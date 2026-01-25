#include "footprint_engine.h"
#include "logger.h"
#include <cmath>
#include <numeric>
#include <algorithm>

namespace shieldorder {

// FootprintBar methods
int FootprintBar::count_stacked_buy_imbalance(double threshold) const {
    int max_stack = 0, current_stack = 0;
    double prev_price = 0.0;

    for (const auto& [price, level] : levels) {
        if (level.is_buy_imbalance(threshold)) {
            if (prev_price == 0.0 || std::abs(price - prev_price) <= 1.5) {
                current_stack++;
                max_stack = std::max(max_stack, current_stack);
            } else {
                current_stack = 1;
            }
        } else {
            current_stack = 0;
        }
        prev_price = price;
    }
    return max_stack;
}

int FootprintBar::count_stacked_sell_imbalance(double threshold) const {
    int max_stack = 0, current_stack = 0;
    double prev_price = 0.0;

    for (const auto& [price, level] : levels) {
        if (level.is_sell_imbalance(threshold)) {
            if (prev_price == 0.0 || std::abs(price - prev_price) <= 1.5) {
                current_stack++;
                max_stack = std::max(max_stack, current_stack);
            } else {
                current_stack = 1;
            }
        } else {
            current_stack = 0;
        }
        prev_price = price;
    }
    return max_stack;
}

double FootprintBar::poc() const {
    double poc_price = 0.0;
    int max_vol = 0;
    for (const auto& [price, level] : levels) {
        if (level.total_volume > max_vol) {
            max_vol = level.total_volume;
            poc_price = price;
        }
    }
    return poc_price;
}

bool FootprintBar::has_delta_divergence(const FootprintBar& prev) const {
    // Bearish divergence: price higher high, delta lower high
    if (high > prev.high && delta < prev.max_delta) return true;
    // Bullish divergence: price lower low, delta higher low
    if (low < prev.low && delta > prev.min_delta) return true;
    return false;
}

// FootprintEngine implementation
FootprintEngine::FootprintEngine(const Config& config)
    : config_(config) {}

void FootprintEngine::on_trade(const ClassifiedTrade& trade) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Start new bar if needed
    if (!bar_started_) {
        current_bar_ = FootprintBar{};
        current_bar_.start_time = trade.time;
        current_bar_.open = trade.price;
        current_bar_.high = trade.price;
        current_bar_.low = trade.price;
        current_bar_.close = trade.price;
        bar_started_ = true;
    }

    // Check if bar period elapsed
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        trade.time - current_bar_.start_time).count();
    if (elapsed >= config_.bar_duration_ms) {
        complete_bar();
        // Start new bar with this trade
        current_bar_ = FootprintBar{};
        current_bar_.start_time = trade.time;
        current_bar_.open = trade.price;
        current_bar_.high = trade.price;
        current_bar_.low = trade.price;
    }

    // Update current bar
    current_bar_.high = std::max(current_bar_.high, trade.price);
    current_bar_.low = std::min(current_bar_.low, trade.price);
    current_bar_.close = trade.price;
    current_bar_.total_volume += trade.size;

    // Update footprint level
    double tick_price = round_to_tick(trade.price);
    auto& level = current_bar_.levels[tick_price];
    level.price = tick_price;
    level.total_volume += trade.size;
    level.trade_count++;
    level.max_single_trade = std::max(level.max_single_trade, trade.size);

    if (trade.aggressor == TradeAggressor::BUYER) {
        level.buy_volume += trade.size;
        current_bar_.delta += trade.size;
    } else if (trade.aggressor == TradeAggressor::SELLER) {
        level.sell_volume += trade.size;
        current_bar_.delta -= trade.size;
    }
    level.delta = level.buy_volume - level.sell_volume;

    // Track delta extremes within bar
    current_bar_.max_delta = std::max(current_bar_.max_delta, current_bar_.delta);
    current_bar_.min_delta = std::min(current_bar_.min_delta, current_bar_.delta);
}

int FootprintEngine::rolling_delta(int bars) const {
    std::lock_guard<std::mutex> lock(mutex_);
    int delta = 0;
    int count = std::min(bars, (int)history_.size());
    for (int i = history_.size() - count; i < (int)history_.size(); ++i) {
        delta += history_[i].delta;
    }
    // Include current bar
    delta += current_bar_.delta;
    return delta;
}

bool FootprintEngine::is_delta_divergence() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (history_.size() < 2) return false;
    return current_bar_.has_delta_divergence(history_.back());
}

bool FootprintEngine::has_stacked_buy_imbalance() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_bar_.count_stacked_buy_imbalance(config_.imbalance_threshold)
           >= config_.stacked_imbalance_min;
}

bool FootprintEngine::has_stacked_sell_imbalance() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_bar_.count_stacked_sell_imbalance(config_.imbalance_threshold)
           >= config_.stacked_imbalance_min;
}

std::vector<double> FootprintEngine::get_buy_imbalance_prices() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<double> prices;
    for (const auto& [price, level] : current_bar_.levels) {
        if (level.is_buy_imbalance(config_.imbalance_threshold)) {
            prices.push_back(price);
        }
    }
    return prices;
}

std::vector<double> FootprintEngine::get_sell_imbalance_prices() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<double> prices;
    for (const auto& [price, level] : current_bar_.levels) {
        if (level.is_sell_imbalance(config_.imbalance_threshold)) {
            prices.push_back(price);
        }
    }
    return prices;
}

std::vector<double> FootprintEngine::get_unfinished_business() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<double> prices;

    if (history_.size() < 2) return prices;

    // Unfinished business: the extreme of a bar that wasn't revisited
    const auto& prev = history_.back();
    bool high_revisited = current_bar_.high >= prev.high;
    bool low_revisited = current_bar_.low <= prev.low;

    if (!high_revisited && prev.high > current_bar_.close) {
        prices.push_back(prev.high);
    }
    if (!low_revisited && prev.low < current_bar_.close) {
        prices.push_back(prev.low);
    }

    return prices;
}

bool FootprintEngine::is_delta_extreme_high(int lookback) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if ((int)history_.size() < lookback) return false;

    int current = current_bar_.delta;
    int count_higher = 0;
    int start = std::max(0, (int)history_.size() - lookback);
    for (int i = start; i < (int)history_.size(); ++i) {
        if (history_[i].delta >= current) count_higher++;
    }
    // Current delta is in top 10% = extreme
    return count_higher < lookback / 10;
}

bool FootprintEngine::is_delta_extreme_low(int lookback) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if ((int)history_.size() < lookback) return false;

    int current = current_bar_.delta;
    int count_lower = 0;
    int start = std::max(0, (int)history_.size() - lookback);
    for (int i = start; i < (int)history_.size(); ++i) {
        if (history_[i].delta <= current) count_lower++;
    }
    return count_lower < lookback / 10;
}

void FootprintEngine::set_bar_callback(std::function<void(const FootprintBar&)> cb) {
    bar_callback_ = cb;
}

void FootprintEngine::complete_bar() {
    current_bar_.end_time = std::chrono::steady_clock::now();

    history_.push_back(current_bar_);
    if ((int)history_.size() > config_.max_history_bars) {
        history_.pop_front();
    }

    if (bar_callback_) {
        bar_callback_(current_bar_);
    }
}

double FootprintEngine::round_to_tick(double price) const {
    return std::round(price / config_.tick_size) * config_.tick_size;
}

} // namespace shieldorder
