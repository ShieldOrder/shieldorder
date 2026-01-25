#include "scalp_engine.h"
#include "logger.h"
#include <cmath>
#include <algorithm>

namespace shieldorder {

ScalpEngine::ScalpEngine(const Config& config)
    : config_(config) {}

ScalpSignal ScalpEngine::evaluate(const std::string& symbol,
                                   const TapeReader& tape,
                                   const FootprintEngine& footprint,
                                   const BookAnalyzer& book,
                                   const MarketStructure& structure,
                                   const VolumeProfile& profile,
                                   double current_price) {
    ScalpSignal signal;
    signal.confidence = 0.0;
    signal.generated_at = std::chrono::steady_clock::now();

    // Check entry filters first
    if (!passes_entry_filters(tape, book, current_price)) {
        return signal;
    }

    // Score each factor for LONG
    double of_long = score_order_flow(footprint, Side::BUY);
    double book_long = score_book(book, Side::BUY);
    double struct_long = score_structure(structure, Side::BUY, current_price);
    double tape_long = score_tape(tape, Side::BUY);
    double vol_long = score_volume_context(profile, Side::BUY, current_price);

    // Score each factor for SHORT
    double of_short = score_order_flow(footprint, Side::SELL);
    double book_short = score_book(book, Side::SELL);
    double struct_short = score_structure(structure, Side::SELL, current_price);
    double tape_short = score_tape(tape, Side::SELL);
    double vol_short = score_volume_context(profile, Side::SELL, current_price);

    // Compute weighted confluence scores
    double long_score = config_.weight_order_flow * of_long +
                        config_.weight_book * book_long +
                        config_.weight_structure * struct_long +
                        config_.weight_tape * tape_long +
                        config_.weight_volume * vol_long;

    double short_score = config_.weight_order_flow * of_short +
                         config_.weight_book * book_short +
                         config_.weight_structure * struct_short +
                         config_.weight_tape * tape_short +
                         config_.weight_volume * vol_short;

    // Determine direction
    bool go_long = long_score > short_score && long_score >= config_.min_confluence;
    bool go_short = short_score > long_score && short_score >= config_.min_confluence;

    if (!go_long && !go_short) {
        return signal;  // No trade
    }

    Side side = go_long ? Side::BUY : Side::SELL;
    double conf = go_long ? long_score : short_score;
    int aligned = go_long ?
        count_aligned_factors(of_long, book_long, struct_long, tape_long, vol_long) :
        count_aligned_factors(of_short, book_short, struct_short, tape_short, vol_short);

    // Require minimum factors aligned
    if (aligned < config_.min_factors_aligned) {
        return signal;
    }

    // Build the signal
    signal.side = side;
    signal.confidence = conf;
    signal.entry_price = current_price;
    signal.stop_price = compute_stop(side, current_price, structure);
    signal.target_price = compute_target(side, current_price, structure, profile);

    signal.order_flow_score = go_long ? of_long : of_short;
    signal.book_score = go_long ? book_long : book_short;
    signal.structure_score = go_long ? struct_long : struct_short;
    signal.tape_score = go_long ? tape_long : tape_short;
    signal.volume_score = go_long ? vol_long : vol_short;

    signal.trigger_reason = (side == Side::BUY ? "LONG" : "SHORT");
    signal.trigger_reason += " conf=" + std::to_string(conf).substr(0,4);
    signal.trigger_reason += " aligned=" + std::to_string(aligned) + "/5";

    LOG_INFO("ScalpEngine", signal.trigger_reason +
             " OF=" + std::to_string(signal.order_flow_score).substr(0,4) +
             " BK=" + std::to_string(signal.book_score).substr(0,4) +
             " ST=" + std::to_string(signal.structure_score).substr(0,4) +
             " TP=" + std::to_string(signal.tape_score).substr(0,4) +
             " VL=" + std::to_string(signal.volume_score).substr(0,4));

    return signal;
}

ScalpEngine::ExitSignal ScalpEngine::check_exit(const std::string& symbol,
                                                 Side position_side,
                                                 double entry_price,
                                                 double current_price,
                                                 Timestamp entry_time,
                                                 const TapeReader& tape,
                                                 const FootprintEngine& footprint,
                                                 const BookAnalyzer& book) {
    ExitSignal exit;
    exit.should_exit = false;

    double pnl_pts = (position_side == Side::BUY) ?
        (current_price - entry_price) : (entry_price - current_price);

    // 1. Target hit
    if (pnl_pts >= config_.default_target_pts) {
        exit.should_exit = true;
        exit.reason = "TARGET HIT +" + std::to_string(pnl_pts) + " pts";
        exit.suggested_exit_price = current_price;
        return exit;
    }

    // 2. Stop hit
    if (pnl_pts <= -config_.tight_stop_pts) {
        exit.should_exit = true;
        exit.reason = "STOP HIT " + std::to_string(pnl_pts) + " pts";
        exit.suggested_exit_price = current_price;
        return exit;
    }

    // 3. Time stop
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - entry_time).count();
    if (elapsed > config_.time_stop_seconds && pnl_pts < config_.breakeven_trigger_pts / 2.0) {
        exit.should_exit = true;
        exit.reason = "TIME STOP after " + std::to_string(elapsed) + "s, pnl=" + std::to_string(pnl_pts);
        exit.suggested_exit_price = current_price;
        return exit;
    }

    // 4. Opposing signal (book flip, delta reversal)
    double book_pressure = book.book_pressure_score();
    bool book_flipped = (position_side == Side::BUY && book_pressure < -0.4) ||
                        (position_side == Side::SELL && book_pressure > 0.4);
    if (book_flipped && pnl_pts > 0) {
        exit.should_exit = true;
        exit.reason = "BOOK FLIP - taking profit +" + std::to_string(pnl_pts);
        exit.suggested_exit_price = current_price;
        return exit;
    }

    // 5. Exhaustion detected
    auto metrics = tape.get_metrics();
    bool exhaustion = (position_side == Side::BUY && metrics.is_exhaustion_buying()) ||
                      (position_side == Side::SELL && metrics.is_exhaustion_selling());
    if (exhaustion && pnl_pts > config_.breakeven_trigger_pts / 2.0) {
        exit.should_exit = true;
        exit.reason = "EXHAUSTION - locking profit +" + std::to_string(pnl_pts);
        exit.suggested_exit_price = current_price;
        return exit;
    }

    // 6. Delta divergence
    int delta = footprint.cumulative_delta();
    bool delta_diverging = (position_side == Side::BUY && delta < 0 && pnl_pts < 2.0) ||
                           (position_side == Side::SELL && delta > 0 && pnl_pts < 2.0);
    if (delta_diverging && elapsed > 15) {
        exit.should_exit = true;
        exit.reason = "DELTA DIVERGENCE - cutting early";
        exit.suggested_exit_price = current_price;
        return exit;
    }

    return exit;
}

void ScalpEngine::record_trade_result(double pnl_points, double confidence_at_entry) {
    stats_.total_trades++;
    stats_.total_pnl += pnl_points;

    if (pnl_points > 0) {
        stats_.winners++;
        stats_.avg_winner = (stats_.avg_winner * (stats_.winners - 1) + pnl_points) / stats_.winners;
    } else {
        stats_.losers++;
        stats_.avg_loser = (stats_.avg_loser * (stats_.losers - 1) + pnl_points) / stats_.losers;
    }

    LOG_INFO("ScalpEngine", "Trade result: " + std::to_string(pnl_points) + " pts, "
             "Win rate: " + std::to_string(stats_.win_rate() * 100).substr(0,4) + "%, "
             "Total P&L: " + std::to_string(stats_.total_pnl) + " pts");
}

double ScalpEngine::score_order_flow(const FootprintEngine& footprint, Side side) const {
    double score = 0.0;

    int delta = footprint.cumulative_delta();
    int bar_delta = footprint.current_bar_delta();

    // Delta confirming direction
    if (side == Side::BUY) {
        if (delta > 0) score += 0.3;
        if (bar_delta > 0) score += 0.3;
        if (footprint.has_stacked_buy_imbalance()) score += 0.3;
        if (footprint.is_delta_accelerating()) score += 0.1;
    } else {
        if (delta < 0) score += 0.3;
        if (bar_delta < 0) score += 0.3;
        if (footprint.has_stacked_sell_imbalance()) score += 0.3;
        if (footprint.is_delta_decelerating()) score += 0.1;
    }

    return std::clamp(score, 0.0, 1.0);
}

double ScalpEngine::score_book(const BookAnalyzer& book, Side side) const {
    double score = 0.0;
    double pressure = book.book_pressure_score();

    if (side == Side::BUY) {
        if (pressure > 0.2) score += 0.3;
        if (book.is_bid_stacking()) score += 0.2;
        if (book.is_ask_pulling()) score += 0.2;
        if (book.is_absorption_at_bid()) score += 0.2;  // Buyers absorbing selling
        if (book.has_iceberg_bid()) score += 0.1;
    } else {
        if (pressure < -0.2) score += 0.3;
        if (book.is_ask_stacking()) score += 0.2;
        if (book.is_bid_pulling()) score += 0.2;
        if (book.is_absorption_at_ask()) score += 0.2;
        if (book.has_iceberg_ask()) score += 0.1;
    }

    return std::clamp(score, 0.0, 1.0);
}

double ScalpEngine::score_structure(const MarketStructure& structure, Side side, double price) const {
    double score = 0.0;
    auto trend = structure.current_trend();

    if (side == Side::BUY) {
        if (trend == MarketStructure::Trend::BULLISH) score += 0.4;
        if (structure.is_higher_low()) score += 0.2;
        if (structure.is_in_bullish_fvg(price)) score += 0.2;
        if (structure.is_near_sell_liquidity(price, 5.0)) score += 0.2;  // Near stop hunt target
    } else {
        if (trend == MarketStructure::Trend::BEARISH) score += 0.4;
        if (structure.is_lower_high()) score += 0.2;
        if (structure.is_in_bearish_fvg(price)) score += 0.2;
        if (structure.is_near_buy_liquidity(price, 5.0)) score += 0.2;
    }

    // Penalize trading against obvious levels
    double resistance = structure.nearest_resistance(price);
    double support = structure.nearest_support(price);

    if (side == Side::BUY && resistance > 0 && (resistance - price) < 3.0) {
        score -= 0.3;  // Buying into resistance
    }
    if (side == Side::SELL && support > 0 && (price - support) < 3.0) {
        score -= 0.3;  // Selling into support
    }

    return std::clamp(score, 0.0, 1.0);
}

double ScalpEngine::score_tape(const TapeReader& tape, Side side) const {
    double score = 0.0;
    auto metrics = tape.get_metrics();

    if (side == Side::BUY) {
        if (metrics.buy_pressure > 0.6) score += 0.3;
        if (metrics.consecutive_buy_ticks > 3) score += 0.2;
        if (tape.recent_large_buys(5000) >= 2) score += 0.3;
        if (tape.is_absorption_at_bid()) score += 0.2;  // Absorption = support holding
    } else {
        if (metrics.sell_pressure > 0.6) score += 0.3;
        if (metrics.consecutive_sell_ticks > 3) score += 0.2;
        if (tape.recent_large_sells(5000) >= 2) score += 0.3;
        if (tape.is_absorption_at_ask()) score += 0.2;
    }

    // Penalize chasing
    if (side == Side::BUY && metrics.velocity > config_.max_velocity) score -= 0.3;
    if (side == Side::SELL && metrics.velocity < -config_.max_velocity) score -= 0.3;

    return std::clamp(score, 0.0, 1.0);
}

double ScalpEngine::score_volume_context(const VolumeProfile& profile, Side side, double price) const {
    double score = 0.0;

    bool at_poc = profile.is_at_poc(price, 3.0);
    bool in_va = profile.is_in_value_area(price);
    bool above_va = price > profile.value_area_high;
    bool below_va = price < profile.value_area_low;

    if (side == Side::BUY) {
        // Buying below value = discount
        if (below_va) score += 0.4;
        // Buying at POC with momentum = breakout potential
        if (at_poc) score += 0.2;
        // Buying above VA = chasing (slight penalty)
        if (above_va) score -= 0.1;
    } else {
        if (above_va) score += 0.4;
        if (at_poc) score += 0.2;
        if (below_va) score -= 0.1;
    }

    return std::clamp(score, 0.0, 1.0);
}

bool ScalpEngine::passes_entry_filters(const TapeReader& tape, const BookAnalyzer& book, double price) const {
    auto metrics = tape.get_metrics();

    // Don't chase fast moves
    if (std::abs(metrics.velocity) > config_.max_velocity) {
        return false;
    }

    // Don't enter during climactic activity (likely reversal coming)
    if (config_.avoid_climactic) {
        if (metrics.is_climactic_buying() || metrics.is_climactic_selling()) {
            return false;
        }
    }

    // Check spread isn't too wide
    auto snapshot = book.current_snapshot();
    if (snapshot.spread() > config_.max_entry_spread_ticks) {
        return false;
    }

    return true;
}

double ScalpEngine::compute_stop(Side side, double entry, const MarketStructure& structure) const {
    double structure_stop;

    if (side == Side::BUY) {
        double support = structure.nearest_support(entry);
        structure_stop = (support > 0) ? support - 1.0 : entry - config_.tight_stop_pts;
    } else {
        double resistance = structure.nearest_resistance(entry);
        structure_stop = (resistance > 0) ? resistance + 1.0 : entry + config_.tight_stop_pts;
    }

    // Use tighter of structure stop or fixed stop
    double fixed_stop = (side == Side::BUY) ?
        entry - config_.tight_stop_pts : entry + config_.tight_stop_pts;

    if (side == Side::BUY) {
        return std::max(structure_stop, fixed_stop);  // Higher stop = tighter
    } else {
        return std::min(structure_stop, fixed_stop);  // Lower stop = tighter
    }
}

double ScalpEngine::compute_target(Side side, double entry, const MarketStructure& structure,
                                    const VolumeProfile& profile) const {
    double target;

    if (side == Side::BUY) {
        double resistance = structure.nearest_resistance(entry);
        if (resistance > 0 && (resistance - entry) >= config_.default_target_pts * 0.8) {
            target = resistance - 1.0;  // Just before resistance
        } else if (profile.poc_price > entry) {
            target = profile.poc_price;  // Target POC
        } else {
            target = entry + config_.default_target_pts;
        }
    } else {
        double support = structure.nearest_support(entry);
        if (support > 0 && (entry - support) >= config_.default_target_pts * 0.8) {
            target = support + 1.0;
        } else if (profile.poc_price < entry) {
            target = profile.poc_price;
        } else {
            target = entry - config_.default_target_pts;
        }
    }

    return target;
}

int ScalpEngine::count_aligned_factors(double of_score, double book_score,
                                        double struct_score, double tape_score,
                                        double vol_score) const {
    int count = 0;
    if (of_score >= 0.3) count++;
    if (book_score >= 0.3) count++;
    if (struct_score >= 0.3) count++;
    if (tape_score >= 0.3) count++;
    if (vol_score >= 0.2) count++;
    return count;
}

} // namespace shieldorder
