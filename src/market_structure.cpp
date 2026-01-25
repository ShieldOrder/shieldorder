#include "market_structure.h"
#include "logger.h"
#include <cmath>
#include <algorithm>

namespace shieldorder {

MarketStructure::MarketStructure(const Config& config)
    : config_(config) {}

void MarketStructure::on_bar(double open, double high, double low, double close, Timestamp time) {
    std::lock_guard<std::mutex> lock(mutex_);

    PriceBar bar{open, high, low, close, time};
    bars_.push_back(bar);

    // Keep last 200 bars
    if (bars_.size() > 200) bars_.pop_front();

    detect_swing_points();
    update_liquidity_levels();
    detect_fvgs();
    check_broken_levels(close);
    check_filled_fvgs(high, low);
}

SwingPoint MarketStructure::last_swing_high() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = swings_.rbegin(); it != swings_.rend(); ++it) {
        if (it->type == SwingType::SWING_HIGH) return *it;
    }
    return SwingPoint{0, {}, SwingType::SWING_HIGH, 0};
}

SwingPoint MarketStructure::last_swing_low() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = swings_.rbegin(); it != swings_.rend(); ++it) {
        if (it->type == SwingType::SWING_LOW) return *it;
    }
    return SwingPoint{0, {}, SwingType::SWING_LOW, 0};
}

bool MarketStructure::is_higher_high() const {
    std::lock_guard<std::mutex> lock(mutex_);
    SwingPoint prev_high{}, curr_high{};
    int found = 0;
    for (auto it = swings_.rbegin(); it != swings_.rend(); ++it) {
        if (it->type == SwingType::SWING_HIGH) {
            if (found == 0) { curr_high = *it; found++; }
            else if (found == 1) { prev_high = *it; found++; break; }
        }
    }
    return found == 2 && curr_high.price > prev_high.price;
}

bool MarketStructure::is_lower_low() const {
    std::lock_guard<std::mutex> lock(mutex_);
    SwingPoint prev_low{}, curr_low{};
    int found = 0;
    for (auto it = swings_.rbegin(); it != swings_.rend(); ++it) {
        if (it->type == SwingType::SWING_LOW) {
            if (found == 0) { curr_low = *it; found++; }
            else if (found == 1) { prev_low = *it; found++; break; }
        }
    }
    return found == 2 && curr_low.price < prev_low.price;
}

bool MarketStructure::is_higher_low() const {
    std::lock_guard<std::mutex> lock(mutex_);
    SwingPoint prev_low{}, curr_low{};
    int found = 0;
    for (auto it = swings_.rbegin(); it != swings_.rend(); ++it) {
        if (it->type == SwingType::SWING_LOW) {
            if (found == 0) { curr_low = *it; found++; }
            else if (found == 1) { prev_low = *it; found++; break; }
        }
    }
    return found == 2 && curr_low.price > prev_low.price;
}

bool MarketStructure::is_lower_high() const {
    std::lock_guard<std::mutex> lock(mutex_);
    SwingPoint prev_high{}, curr_high{};
    int found = 0;
    for (auto it = swings_.rbegin(); it != swings_.rend(); ++it) {
        if (it->type == SwingType::SWING_HIGH) {
            if (found == 0) { curr_high = *it; found++; }
            else if (found == 1) { prev_high = *it; found++; break; }
        }
    }
    return found == 2 && curr_high.price < prev_high.price;
}

MarketStructure::Trend MarketStructure::current_trend() const {
    // Bullish: HH + HL
    bool hh = is_higher_high();
    bool hl = is_higher_low();
    bool lh = is_lower_high();
    bool ll = is_lower_low();

    if (hh && hl) return Trend::BULLISH;
    if (lh && ll) return Trend::BEARISH;
    return Trend::RANGING;
}

bool MarketStructure::is_near_buy_liquidity(double price, double tolerance) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& level : liquidity_) {
        if (level.is_buy_side && std::abs(price - level.price) <= tolerance) return true;
    }
    return false;
}

bool MarketStructure::is_near_sell_liquidity(double price, double tolerance) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& level : liquidity_) {
        if (!level.is_buy_side && std::abs(price - level.price) <= tolerance) return true;
    }
    return false;
}

bool MarketStructure::is_in_bullish_fvg(double price) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& fvg : fvgs_) {
        if (fvg.is_bullish && !fvg.filled && price >= fvg.low && price <= fvg.high) return true;
    }
    return false;
}

bool MarketStructure::is_in_bearish_fvg(double price) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& fvg : fvgs_) {
        if (!fvg.is_bullish && !fvg.filled && price >= fvg.low && price <= fvg.high) return true;
    }
    return false;
}

FairValueGap MarketStructure::nearest_unfilled_fvg(double price) const {
    std::lock_guard<std::mutex> lock(mutex_);
    FairValueGap nearest{};
    double min_dist = 1e9;

    for (const auto& fvg : fvgs_) {
        if (fvg.filled) continue;
        double mid = (fvg.high + fvg.low) / 2.0;
        double dist = std::abs(price - mid);
        if (dist < min_dist) {
            min_dist = dist;
            nearest = fvg;
        }
    }
    return nearest;
}

double MarketStructure::nearest_support(double price) const {
    std::lock_guard<std::mutex> lock(mutex_);
    double best = 0.0;

    // Check swing lows below current price
    for (const auto& swing : swings_) {
        if (swing.type == SwingType::SWING_LOW && swing.price < price && !swing.broken) {
            if (swing.price > best) best = swing.price;
        }
    }

    // Check liquidity levels
    for (const auto& level : liquidity_) {
        if (!level.is_buy_side && level.price < price) {
            if (level.price > best) best = level.price;
        }
    }

    return best;
}

double MarketStructure::nearest_resistance(double price) const {
    std::lock_guard<std::mutex> lock(mutex_);
    double best = 1e9;

    for (const auto& swing : swings_) {
        if (swing.type == SwingType::SWING_HIGH && swing.price > price && !swing.broken) {
            if (swing.price < best) best = swing.price;
        }
    }

    for (const auto& level : liquidity_) {
        if (level.is_buy_side && level.price > price) {
            if (level.price < best) best = level.price;
        }
    }

    return (best < 1e8) ? best : 0.0;
}

void MarketStructure::detect_swing_points() {
    int n = bars_.size();
    int strength = config_.swing_strength;
    if (n < strength * 2 + 1) return;

    int pivot_idx = n - strength - 1;
    const auto& pivot = bars_[pivot_idx];

    // Check for swing high
    bool is_high = true;
    for (int i = pivot_idx - strength; i <= pivot_idx + strength; ++i) {
        if (i == pivot_idx) continue;
        if (i < 0 || i >= n) { is_high = false; break; }
        if (bars_[i].high >= pivot.high) { is_high = false; break; }
    }

    if (is_high) {
        // Don't add duplicate
        bool duplicate = false;
        for (const auto& s : swings_) {
            if (s.type == SwingType::SWING_HIGH && std::abs(s.price - pivot.high) < 0.5) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            SwingPoint sp;
            sp.price = pivot.high;
            sp.time = pivot.time;
            sp.type = SwingType::SWING_HIGH;
            sp.strength = strength;
            swings_.push_back(sp);
        }
    }

    // Check for swing low
    bool is_low = true;
    for (int i = pivot_idx - strength; i <= pivot_idx + strength; ++i) {
        if (i == pivot_idx) continue;
        if (i < 0 || i >= n) { is_low = false; break; }
        if (bars_[i].low <= pivot.low) { is_low = false; break; }
    }

    if (is_low) {
        bool duplicate = false;
        for (const auto& s : swings_) {
            if (s.type == SwingType::SWING_LOW && std::abs(s.price - pivot.low) < 0.5) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            SwingPoint sp;
            sp.price = pivot.low;
            sp.time = pivot.time;
            sp.type = SwingType::SWING_LOW;
            sp.strength = strength;
            swings_.push_back(sp);
        }
    }

    // Trim old swing points
    while ((int)swings_.size() > config_.max_swing_points) {
        swings_.pop_front();
    }
}

void MarketStructure::update_liquidity_levels() {
    // Liquidity pools form above swing highs (buy stops) and below swing lows (sell stops)
    liquidity_.clear();

    for (const auto& swing : swings_) {
        if (swing.broken) continue;

        LiquidityLevel level;
        level.first_seen = swing.time;
        level.times_tested = 0;

        if (swing.type == SwingType::SWING_HIGH) {
            // Buy stops cluster above swing highs
            level.price = swing.price + config_.liquidity_zone_width / 2.0;
            level.is_buy_side = true;
            level.strength = swing.strength * 0.2;
        } else {
            // Sell stops cluster below swing lows
            level.price = swing.price - config_.liquidity_zone_width / 2.0;
            level.is_buy_side = false;
            level.strength = swing.strength * 0.2;
        }

        liquidity_.push_back(level);
    }

    // Limit size
    if ((int)liquidity_.size() > config_.max_liquidity_levels) {
        liquidity_.erase(liquidity_.begin(),
                        liquidity_.begin() + (liquidity_.size() - config_.max_liquidity_levels));
    }
}

void MarketStructure::detect_fvgs() {
    int n = bars_.size();
    if (n < 3) return;

    const auto& bar1 = bars_[n-3];  // Two bars ago
    const auto& bar2 = bars_[n-2];  // Previous bar (the gap bar)
    const auto& bar3 = bars_[n-1];  // Current bar

    // Bullish FVG: bar1.high < bar3.low (gap between bar1 high and bar3 low)
    if (bar3.low > bar1.high) {
        double gap_size = bar3.low - bar1.high;
        if (gap_size >= config_.fvg_min_size_ticks) {
            // Check it's not a duplicate
            bool duplicate = false;
            for (const auto& fvg : fvgs_) {
                if (std::abs(fvg.low - bar1.high) < 0.5 && std::abs(fvg.high - bar3.low) < 0.5) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                FairValueGap fvg;
                fvg.low = bar1.high;
                fvg.high = bar3.low;
                fvg.is_bullish = true;
                fvg.time = bar2.time;
                fvgs_.push_back(fvg);
            }
        }
    }

    // Bearish FVG: bar1.low > bar3.high (gap between bar3 high and bar1 low)
    if (bar1.low > bar3.high) {
        double gap_size = bar1.low - bar3.high;
        if (gap_size >= config_.fvg_min_size_ticks) {
            bool duplicate = false;
            for (const auto& fvg : fvgs_) {
                if (std::abs(fvg.low - bar3.high) < 0.5 && std::abs(fvg.high - bar1.low) < 0.5) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                FairValueGap fvg;
                fvg.low = bar3.high;
                fvg.high = bar1.low;
                fvg.is_bullish = false;
                fvg.time = bar2.time;
                fvgs_.push_back(fvg);
            }
        }
    }

    while ((int)fvgs_.size() > config_.max_fvgs) {
        fvgs_.pop_front();
    }
}

void MarketStructure::check_broken_levels(double current_price) {
    for (auto& swing : swings_) {
        if (swing.broken) continue;
        if (swing.type == SwingType::SWING_HIGH &&
            current_price > swing.price + config_.broken_level_buffer) {
            swing.broken = true;
        }
        if (swing.type == SwingType::SWING_LOW &&
            current_price < swing.price - config_.broken_level_buffer) {
            swing.broken = true;
        }
    }
}

void MarketStructure::check_filled_fvgs(double high, double low) {
    for (auto& fvg : fvgs_) {
        if (fvg.filled) continue;
        if (fvg.is_bullish) {
            // Bullish FVG filled when price trades down into it
            if (low <= fvg.low) fvg.filled = true;
        } else {
            // Bearish FVG filled when price trades up into it
            if (high >= fvg.high) fvg.filled = true;
        }
    }
}

} // namespace shieldorder
