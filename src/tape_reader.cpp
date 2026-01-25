#include "tape_reader.h"
#include "logger.h"
#include <algorithm>
#include <numeric>
#include <cmath>

namespace shieldorder {

TapeReader::TapeReader(const Config& config)
    : config_(config) {}

void TapeReader::on_tick(const Tick& tick) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (tick.last_size == 0) return;  // Quote-only update

    ClassifiedTrade trade;
    trade.price = tick.last;
    trade.size = tick.last_size;
    trade.aggressor = classify_aggressor(tick);
    trade.size_class = classify_size(tick.last_size);
    trade.time = tick.local_time;
    trade.sequence = ++sequence_;
    trade.bid_at_trade = tick.bid;
    trade.ask_at_trade = tick.ask;
    trade.bid_size_at_trade = tick.bid_size;
    trade.ask_size_at_trade = tick.ask_size;

    // Update cumulative delta
    if (trade.aggressor == TradeAggressor::BUYER) {
        cumulative_delta_ += trade.size;
        session_delta_ += trade.size;
    } else if (trade.aggressor == TradeAggressor::SELLER) {
        cumulative_delta_ -= trade.size;
        session_delta_ -= trade.size;
    }

    // Update consecutive tick tracking
    update_consecutive_ticks(trade.price);

    // Update average trade size (exponential moving average)
    avg_trade_size_ = 0.99 * avg_trade_size_ + 0.01 * trade.size;

    // Update absorption tracking
    update_absorption(trade);

    // Store trade
    trades_.push_back(trade);
    trim_old_trades();

    // Notify callback for large/institutional trades
    if (trade.size_class >= TradeSize::LARGE && event_callback_) {
        event_callback_(trade);
    }

    last_price_ = trade.price;
    last_trade_time_ = trade.time;
}

TapeMetrics TapeReader::get_metrics() const {
    std::lock_guard<std::mutex> lock(mutex_);

    TapeMetrics metrics{};
    if (trades_.empty()) return metrics;

    auto now = std::chrono::steady_clock::now();
    auto window = std::chrono::milliseconds(config_.metrics_window_ms);
    auto cutoff = now - window;

    int count = 0;
    int volume = 0;
    int buy_vol = 0, sell_vol = 0;
    int large_count = 0;
    double price_start = 0.0, price_end = 0.0;

    for (auto it = trades_.rbegin(); it != trades_.rend(); ++it) {
        if (it->time < cutoff) break;

        count++;
        volume += it->size;

        if (it->aggressor == TradeAggressor::BUYER) buy_vol += it->size;
        else if (it->aggressor == TradeAggressor::SELLER) sell_vol += it->size;

        if (it->size_class >= TradeSize::LARGE) large_count++;

        if (price_end == 0.0) price_end = it->price;
        price_start = it->price;
    }

    double window_seconds = config_.metrics_window_ms / 1000.0;
    metrics.trades_per_second = count / window_seconds;
    metrics.volume_per_second = volume / window_seconds;
    metrics.avg_trade_size = (count > 0) ? (double)volume / count : avg_trade_size_;
    metrics.large_trade_count = large_count;

    int total_vol = buy_vol + sell_vol;
    metrics.buy_pressure = (total_vol > 0) ? (double)buy_vol / total_vol : 0.5;
    metrics.sell_pressure = (total_vol > 0) ? (double)sell_vol / total_vol : 0.5;

    // Velocity: price change per second
    if (count > 1) {
        metrics.velocity = (price_end - price_start) / window_seconds;
    }

    // Acceleration: compare velocity of recent half vs older half
    if (trades_.size() > 10) {
        auto mid_cutoff = now - window / 2;
        double recent_price_start = 0.0, recent_price_end = 0.0;
        double older_price_start = 0.0, older_price_end = 0.0;

        for (auto it = trades_.rbegin(); it != trades_.rend(); ++it) {
            if (it->time < cutoff) break;
            if (it->time >= mid_cutoff) {
                if (recent_price_end == 0.0) recent_price_end = it->price;
                recent_price_start = it->price;
            } else {
                if (older_price_end == 0.0) older_price_end = it->price;
                older_price_start = it->price;
            }
        }

        double half_window = window_seconds / 2.0;
        double recent_vel = (recent_price_end - recent_price_start) / half_window;
        double older_vel = (older_price_end - older_price_start) / half_window;
        metrics.acceleration = (recent_vel - older_vel) / half_window;
    }

    metrics.consecutive_buy_ticks = consecutive_up_;
    metrics.consecutive_sell_ticks = consecutive_down_;

    return metrics;
}

bool TapeReader::is_absorption_at_bid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    // Absorption = high sell volume hitting bid but bid price not dropping
    return bid_absorption_.volume_absorbed > avg_trade_size_ * 10 &&
           bid_absorption_.ticks_at_price > 5;
}

bool TapeReader::is_absorption_at_ask() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ask_absorption_.volume_absorbed > avg_trade_size_ * 10 &&
           ask_absorption_.ticks_at_price > 5;
}

bool TapeReader::is_buy_exhaustion() const {
    auto metrics = get_metrics();
    return metrics.is_exhaustion_buying();
}

bool TapeReader::is_sell_exhaustion() const {
    auto metrics = get_metrics();
    return metrics.is_exhaustion_selling();
}

int TapeReader::recent_large_buys(int window_ms) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto cutoff = std::chrono::steady_clock::now() - std::chrono::milliseconds(window_ms);
    int count = 0;
    for (auto it = trades_.rbegin(); it != trades_.rend(); ++it) {
        if (it->time < cutoff) break;
        if (it->size_class >= TradeSize::LARGE && it->aggressor == TradeAggressor::BUYER) {
            count++;
        }
    }
    return count;
}

int TapeReader::recent_large_sells(int window_ms) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto cutoff = std::chrono::steady_clock::now() - std::chrono::milliseconds(window_ms);
    int count = 0;
    for (auto it = trades_.rbegin(); it != trades_.rend(); ++it) {
        if (it->time < cutoff) break;
        if (it->size_class >= TradeSize::LARGE && it->aggressor == TradeAggressor::SELLER) {
            count++;
        }
    }
    return count;
}

void TapeReader::set_event_callback(std::function<void(const ClassifiedTrade&)> cb) {
    event_callback_ = cb;
}

TradeAggressor TapeReader::classify_aggressor(const Tick& tick) const {
    // Primary: trade at ask = buyer, trade at bid = seller
    if (tick.last >= tick.ask) return TradeAggressor::BUYER;
    if (tick.last <= tick.bid) return TradeAggressor::SELLER;

    // Mid-spread: use tick direction
    if (last_price_ > 0.0) {
        if (tick.last > last_price_) return TradeAggressor::BUYER;
        if (tick.last < last_price_) return TradeAggressor::SELLER;
    }

    // At mid or no direction: classify by which side it's closer to
    double mid = (tick.bid + tick.ask) / 2.0;
    return (tick.last >= mid) ? TradeAggressor::BUYER : TradeAggressor::SELLER;
}

TradeSize TapeReader::classify_size(int size) const {
    if (size >= config_.institutional_size) return TradeSize::INSTITUTIONAL;
    if (size >= avg_trade_size_ * config_.large_trade_multiplier) return TradeSize::LARGE;
    if (size >= avg_trade_size_ * 1.5) return TradeSize::MEDIUM;
    return TradeSize::SMALL;
}

void TapeReader::update_absorption(const ClassifiedTrade& trade) {
    // Track volume absorbed at the bid (selling into the bid)
    if (trade.aggressor == TradeAggressor::SELLER) {
        if (std::abs(trade.price - bid_absorption_.price) < 0.01) {
            bid_absorption_.volume_absorbed += trade.size;
            bid_absorption_.ticks_at_price++;
        } else {
            // Price moved, reset
            bid_absorption_.price = trade.price;
            bid_absorption_.volume_absorbed = trade.size;
            bid_absorption_.ticks_at_price = 1;
            bid_absorption_.start_time = trade.time;
        }
    }

    // Track volume absorbed at the ask (buying into the ask)
    if (trade.aggressor == TradeAggressor::BUYER) {
        if (std::abs(trade.price - ask_absorption_.price) < 0.01) {
            ask_absorption_.volume_absorbed += trade.size;
            ask_absorption_.ticks_at_price++;
        } else {
            ask_absorption_.price = trade.price;
            ask_absorption_.volume_absorbed = trade.size;
            ask_absorption_.ticks_at_price = 1;
            ask_absorption_.start_time = trade.time;
        }
    }
}

void TapeReader::update_consecutive_ticks(double price) {
    if (last_price_ == 0.0) {
        last_price_ = price;
        return;
    }

    if (price > last_price_) {
        consecutive_up_++;
        consecutive_down_ = 0;
    } else if (price < last_price_) {
        consecutive_down_++;
        consecutive_up_ = 0;
    }
    // Equal price: don't reset either counter
}

void TapeReader::trim_old_trades() {
    while ((int)trades_.size() > max_trades_) {
        trades_.pop_front();
    }
}

} // namespace shieldorder
