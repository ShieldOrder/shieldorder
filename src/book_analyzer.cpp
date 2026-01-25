#include "book_analyzer.h"
#include "logger.h"
#include <cmath>
#include <algorithm>
#include <numeric>

namespace shieldorder {

BookAnalyzer::BookAnalyzer(const Config& config)
    : config_(config) {}

void BookAnalyzer::on_depth(const DepthUpdate& update) {
    std::lock_guard<std::mutex> lock(mutex_);

    BookSnapshot prev = current_;

    // Build new snapshot
    current_.bids.clear();
    current_.asks.clear();
    current_.time = std::chrono::steady_clock::now();

    for (const auto& level : update.bids) {
        BookLevel bl;
        bl.price = level.price;
        bl.size = level.size;
        bl.order_count = level.order_count;
        bl.last_update = current_.time;
        bl.size_delta = 0;
        current_.bids.push_back(bl);
    }
    for (const auto& level : update.asks) {
        BookLevel bl;
        bl.price = level.price;
        bl.size = level.size;
        bl.order_count = level.order_count;
        bl.last_update = current_.time;
        bl.size_delta = 0;
        current_.asks.push_back(bl);
    }

    // Compute size deltas vs previous snapshot
    if (!prev.bids.empty()) {
        for (auto& bid : current_.bids) {
            for (const auto& prev_bid : prev.bids) {
                if (std::abs(bid.price - prev_bid.price) < 0.01) {
                    bid.size_delta = bid.size - prev_bid.size;
                    break;
                }
            }
        }
        for (auto& ask : current_.asks) {
            for (const auto& prev_ask : prev.asks) {
                if (std::abs(ask.price - prev_ask.price) < 0.01) {
                    ask.size_delta = ask.size - prev_ask.size;
                    break;
                }
            }
        }

        // Detect events
        detect_stacking_pulling(prev, current_);
        detect_sweeps(prev, current_);
    }

    // Store snapshot
    snapshot_history_.push_back(current_);
    if ((int)snapshot_history_.size() > config_.snapshot_history) {
        snapshot_history_.pop_front();
    }

    trim_old_events();
}

void BookAnalyzer::on_trade(const ClassifiedTrade& trade) {
    std::lock_guard<std::mutex> lock(mutex_);
    detect_icebergs(trade);
    detect_absorption(trade);
}

double BookAnalyzer::imbalance(int levels) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_.imbalance_ratio(levels);
}

std::vector<BookEventRecord> BookAnalyzer::get_recent_events(int max_age_ms) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto cutoff = std::chrono::steady_clock::now() - std::chrono::milliseconds(max_age_ms);
    std::vector<BookEventRecord> result;
    for (const auto& e : events_) {
        if (e.time >= cutoff) result.push_back(e);
    }
    return result;
}

bool BookAnalyzer::is_bid_stacking(double price_range) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_.bids.empty()) return false;

    double best_bid = current_.bids[0].price;
    int total_added = 0;
    for (const auto& bid : current_.bids) {
        if (best_bid - bid.price > price_range) break;
        if (bid.size_delta > 0) total_added += bid.size_delta;
    }
    return total_added > avg_level_size() * config_.significant_size_mult;
}

bool BookAnalyzer::is_ask_stacking(double price_range) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_.asks.empty()) return false;

    double best_ask = current_.asks[0].price;
    int total_added = 0;
    for (const auto& ask : current_.asks) {
        if (ask.price - best_ask > price_range) break;
        if (ask.size_delta > 0) total_added += ask.size_delta;
    }
    return total_added > avg_level_size() * config_.significant_size_mult;
}

bool BookAnalyzer::is_bid_pulling() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_.bids.size() < 3) return false;

    int levels_pulled = 0;
    for (int i = 0; i < std::min(3, (int)current_.bids.size()); ++i) {
        if (current_.bids[i].size_delta < -(int)(avg_level_size() * config_.pull_threshold_pct)) {
            levels_pulled++;
        }
    }
    return levels_pulled >= 2;
}

bool BookAnalyzer::is_ask_pulling() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_.asks.size() < 3) return false;

    int levels_pulled = 0;
    for (int i = 0; i < std::min(3, (int)current_.asks.size()); ++i) {
        if (current_.asks[i].size_delta < -(int)(avg_level_size() * config_.pull_threshold_pct)) {
            levels_pulled++;
        }
    }
    return levels_pulled >= 2;
}

bool BookAnalyzer::is_absorption_at_bid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bid_absorption_state_.refills >= config_.iceberg_refill_count;
}

bool BookAnalyzer::is_absorption_at_ask() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ask_absorption_state_.refills >= config_.iceberg_refill_count;
}

bool BookAnalyzer::has_iceberg_bid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [key, state] : bid_icebergs_) {
        if (state.refill_count >= config_.iceberg_refill_count) return true;
    }
    return false;
}

bool BookAnalyzer::has_iceberg_ask() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [key, state] : ask_icebergs_) {
        if (state.refill_count >= config_.iceberg_refill_count) return true;
    }
    return false;
}

bool BookAnalyzer::is_sweep_up() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto cutoff = std::chrono::steady_clock::now() - std::chrono::milliseconds(config_.sweep_time_ms);
    for (const auto& e : events_) {
        if (e.time >= cutoff && e.event == BookEvent::SWEEP_ASK) return true;
    }
    return false;
}

bool BookAnalyzer::is_sweep_down() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto cutoff = std::chrono::steady_clock::now() - std::chrono::milliseconds(config_.sweep_time_ms);
    for (const auto& e : events_) {
        if (e.time >= cutoff && e.event == BookEvent::SWEEP_BID) return true;
    }
    return false;
}

std::vector<BookLevel> BookAnalyzer::get_significant_bids() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<BookLevel> result;
    double threshold = avg_level_size() * config_.significant_size_mult;
    for (const auto& bid : current_.bids) {
        if (bid.size > threshold) result.push_back(bid);
    }
    return result;
}

std::vector<BookLevel> BookAnalyzer::get_significant_asks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<BookLevel> result;
    double threshold = avg_level_size() * config_.significant_size_mult;
    for (const auto& ask : current_.asks) {
        if (ask.size > threshold) result.push_back(ask);
    }
    return result;
}

double BookAnalyzer::book_pressure_score() const {
    std::lock_guard<std::mutex> lock(mutex_);

    double score = 0.0;

    // Component 1: Raw imbalance (bid/ask depth ratio)
    double imb = current_.imbalance_ratio(config_.imbalance_levels);
    // Normalize: 1.0 = neutral, >1 = bid heavy, <1 = ask heavy
    double imb_score = std::clamp((imb - 1.0) / 2.0, -0.4, 0.4);
    score += imb_score;

    // Component 2: Stacking/pulling dynamics
    if (is_bid_stacking()) score += 0.2;
    if (is_ask_stacking()) score -= 0.2;
    if (is_bid_pulling()) score -= 0.15;
    if (is_ask_pulling()) score += 0.15;

    // Component 3: Absorption
    if (bid_absorption_state_.refills >= 2) score += 0.15;  // Bid absorbing = bullish
    if (ask_absorption_state_.refills >= 2) score -= 0.15;

    // Component 4: Icebergs
    if (has_iceberg_bid()) score += 0.1;
    if (has_iceberg_ask()) score -= 0.1;

    return std::clamp(score, -1.0, 1.0);
}

void BookAnalyzer::set_event_callback(std::function<void(const BookEventRecord&)> cb) {
    event_callback_ = cb;
}

void BookAnalyzer::detect_stacking_pulling(const BookSnapshot& prev, const BookSnapshot& curr) {
    if (prev.bids.empty() || curr.bids.empty()) return;

    double threshold = avg_level_size();

    // Check top 3 bid levels
    for (int i = 0; i < std::min(3, (int)curr.bids.size()); ++i) {
        if (curr.bids[i].size_delta > threshold * config_.stack_threshold_pct) {
            emit_event(BookEvent::BID_STACK, curr.bids[i].price, curr.bids[i].size_delta, 0.6);
        }
        if (curr.bids[i].size_delta < -(int)(threshold * config_.pull_threshold_pct)) {
            emit_event(BookEvent::BID_PULL, curr.bids[i].price, -curr.bids[i].size_delta, 0.5);
        }
    }

    for (int i = 0; i < std::min(3, (int)curr.asks.size()); ++i) {
        if (curr.asks[i].size_delta > threshold * config_.stack_threshold_pct) {
            emit_event(BookEvent::ASK_STACK, curr.asks[i].price, curr.asks[i].size_delta, 0.6);
        }
        if (curr.asks[i].size_delta < -(int)(threshold * config_.pull_threshold_pct)) {
            emit_event(BookEvent::ASK_PULL, curr.asks[i].price, -curr.asks[i].size_delta, 0.5);
        }
    }

    // Book flip detection
    double prev_imb = prev.imbalance_ratio(3);
    double curr_imb = curr.imbalance_ratio(3);
    if ((prev_imb > 1.5 && curr_imb < 0.67) || (prev_imb < 0.67 && curr_imb > 1.5)) {
        emit_event(BookEvent::FLIP, curr.mid_price(), 0, 0.8);
    }
}

void BookAnalyzer::detect_sweeps(const BookSnapshot& prev, const BookSnapshot& curr) {
    // Ask sweep: multiple ask levels disappeared (aggressive buying through)
    if (prev.asks.size() >= 3 && curr.asks.size() >= 1) {
        if (curr.asks[0].price > prev.asks[config_.sweep_levels - 1].price) {
            emit_event(BookEvent::SWEEP_ASK, curr.asks[0].price, 0, 0.9);
        }
    }

    // Bid sweep: multiple bid levels disappeared (aggressive selling through)
    if (prev.bids.size() >= 3 && curr.bids.size() >= 1) {
        if (curr.bids[0].price < prev.bids[config_.sweep_levels - 1].price) {
            emit_event(BookEvent::SWEEP_BID, curr.bids[0].price, 0, 0.9);
        }
    }
}

void BookAnalyzer::detect_icebergs(const ClassifiedTrade& trade) {
    // Iceberg = resting order that refills after being partially traded
    int price_key = (int)(trade.price * 100);

    if (trade.aggressor == TradeAggressor::SELLER) {
        // Buyer is resting at this price, getting hit
        auto& state = bid_icebergs_[price_key];
        if (std::abs(state.price - trade.price) < 0.01) {
            state.refill_count++;
            if (state.refill_count >= config_.iceberg_refill_count) {
                emit_event(BookEvent::ICEBERG_BID, trade.price, trade.size, 0.7);
            }
        } else {
            state.price = trade.price;
            state.refill_count = 1;
            state.first_seen = trade.time;
        }
    }

    if (trade.aggressor == TradeAggressor::BUYER) {
        auto& state = ask_icebergs_[price_key];
        if (std::abs(state.price - trade.price) < 0.01) {
            state.refill_count++;
            if (state.refill_count >= config_.iceberg_refill_count) {
                emit_event(BookEvent::ICEBERG_ASK, trade.price, trade.size, 0.7);
            }
        } else {
            state.price = trade.price;
            state.refill_count = 1;
            state.first_seen = trade.time;
        }
    }
}

void BookAnalyzer::detect_absorption(const ClassifiedTrade& trade) {
    // Absorption = repeated hits at a price level that doesn't give way
    if (trade.aggressor == TradeAggressor::SELLER) {
        if (std::abs(trade.price - bid_absorption_state_.price) < 0.5) {
            bid_absorption_state_.volume_absorbed += trade.size;
            bid_absorption_state_.refills++;
        } else {
            bid_absorption_state_.price = trade.price;
            bid_absorption_state_.volume_absorbed = trade.size;
            bid_absorption_state_.refills = 1;
            bid_absorption_state_.start = trade.time;
        }

        if (bid_absorption_state_.refills >= config_.iceberg_refill_count) {
            emit_event(BookEvent::ABSORPTION_BID, trade.price,
                      bid_absorption_state_.volume_absorbed, 0.8);
        }
    }

    if (trade.aggressor == TradeAggressor::BUYER) {
        if (std::abs(trade.price - ask_absorption_state_.price) < 0.5) {
            ask_absorption_state_.volume_absorbed += trade.size;
            ask_absorption_state_.refills++;
        } else {
            ask_absorption_state_.price = trade.price;
            ask_absorption_state_.volume_absorbed = trade.size;
            ask_absorption_state_.refills = 1;
            ask_absorption_state_.start = trade.time;
        }

        if (ask_absorption_state_.refills >= config_.iceberg_refill_count) {
            emit_event(BookEvent::ABSORPTION_ASK, trade.price,
                      ask_absorption_state_.volume_absorbed, 0.8);
        }
    }
}

void BookAnalyzer::emit_event(BookEvent type, double price, int size, double significance) {
    BookEventRecord record;
    record.event = type;
    record.price = price;
    record.size = size;
    record.time = std::chrono::steady_clock::now();
    record.significance = significance;

    events_.push_back(record);

    if (event_callback_) {
        event_callback_(record);
    }
}

double BookAnalyzer::avg_level_size() const {
    double total = 0.0;
    int count = 0;
    for (const auto& bid : current_.bids) {
        total += bid.size;
        count++;
    }
    for (const auto& ask : current_.asks) {
        total += ask.size;
        count++;
    }
    return (count > 0) ? total / count : 20.0;
}

void BookAnalyzer::trim_old_events() {
    auto cutoff = std::chrono::steady_clock::now() - std::chrono::seconds(30);
    while (!events_.empty() && events_.front().time < cutoff) {
        events_.pop_front();
    }
}

// BookSnapshot methods
int BookSnapshot::bid_depth(int levels) const {
    int total = 0;
    for (int i = 0; i < std::min(levels, (int)bids.size()); ++i) {
        total += bids[i].size;
    }
    return total;
}

int BookSnapshot::ask_depth(int levels) const {
    int total = 0;
    for (int i = 0; i < std::min(levels, (int)asks.size()); ++i) {
        total += asks[i].size;
    }
    return total;
}

double BookSnapshot::imbalance_ratio(int levels) const {
    int bd = bid_depth(levels);
    int ad = ask_depth(levels);
    if (ad == 0) return (bd > 0) ? 10.0 : 1.0;
    return (double)bd / ad;
}

} // namespace shieldorder
