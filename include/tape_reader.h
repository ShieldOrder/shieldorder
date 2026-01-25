#pragma once

#include "order_flow_types.h"
#include <deque>
#include <mutex>
#include <functional>

namespace shieldorder {

class TapeReader {
public:
    struct Config {
        int large_trade_multiplier = 3;     // Trade is "large" if > N * avg_size
        int institutional_size = 50;         // Contracts per trade = institutional
        int metrics_window_ms = 2000;        // Window for tape speed metrics
        int run_detection_ticks = 5;         // Min consecutive ticks for a "run"
        int climactic_window_ms = 5000;      // Window for climactic volume detection
        double absorption_threshold = 0.7;   // Price movement vs volume ratio
    };

    explicit TapeReader(const Config& config = Config{});

    // Process a new tick from the market
    void on_tick(const Tick& tick);

    // Get current tape metrics
    TapeMetrics get_metrics() const;

    // Get recent classified trades
    const std::deque<ClassifiedTrade>& get_recent_trades() const { return trades_; }

    // Get cumulative delta
    int cumulative_delta() const { return cumulative_delta_; }
    int session_delta() const { return session_delta_; }
    void reset_session_delta() { session_delta_ = 0; }

    // Detect absorption: volume hitting a price but price not moving
    bool is_absorption_at_bid() const;
    bool is_absorption_at_ask() const;

    // Detect exhaustion: climactic volume with decelerating price
    bool is_buy_exhaustion() const;
    bool is_sell_exhaustion() const;

    // Large lot tracker
    int recent_large_buys(int window_ms = 5000) const;
    int recent_large_sells(int window_ms = 5000) const;

    // Callback for significant tape events
    void set_event_callback(std::function<void(const ClassifiedTrade&)> cb);

private:
    Config config_;
    mutable std::mutex mutex_;
    std::deque<ClassifiedTrade> trades_;
    int max_trades_ = 5000;

    // Running statistics
    double avg_trade_size_ = 10.0;
    int cumulative_delta_ = 0;
    int session_delta_ = 0;
    uint64_t sequence_ = 0;

    // For tape speed calculation
    double last_price_ = 0.0;
    Timestamp last_trade_time_;

    // For consecutive tick tracking
    int consecutive_up_ = 0;
    int consecutive_down_ = 0;

    // Absorption tracking
    struct AbsorptionState {
        double price = 0.0;
        int volume_absorbed = 0;
        int ticks_at_price = 0;
        Timestamp start_time;
    };
    AbsorptionState bid_absorption_;
    AbsorptionState ask_absorption_;

    std::function<void(const ClassifiedTrade&)> event_callback_;

    TradeAggressor classify_aggressor(const Tick& tick) const;
    TradeSize classify_size(int size) const;
    void update_absorption(const ClassifiedTrade& trade);
    void update_consecutive_ticks(double price);
    void trim_old_trades();
};

} // namespace shieldorder
