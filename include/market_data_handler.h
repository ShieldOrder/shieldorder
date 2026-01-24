#pragma once

#include "types.h"
#include <deque>
#include <mutex>
#include <unordered_map>
#include <functional>

namespace shieldorder {

struct BarData {
    double open, high, low, close;
    uint64_t volume;
    int tick_count;
    Timestamp start_time;
    Timestamp end_time;
};

struct MarketState {
    Tick last_tick;
    BarData current_bar;
    std::deque<BarData> bar_history;  // Recent completed bars
    double vwap = 0.0;
    double cumulative_delta = 0.0;    // Buy volume - sell volume
    double spread_avg = 0.0;
    int depth_imbalance = 0;          // Bid depth - ask depth at top levels
    uint64_t session_volume = 0;
};

class MarketDataHandler {
public:
    explicit MarketDataHandler(int aggregation_ms, int max_history_bars = 200);

    // Process incoming data
    void on_tick(const Tick& tick);
    void on_depth(const DepthUpdate& update);

    // Query market state
    const MarketState& get_state(const std::string& symbol) const;
    double get_mid_price(const std::string& symbol) const;
    double get_spread(const std::string& symbol) const;
    bool has_data(const std::string& symbol) const;

    // Correlation between two instruments
    double compute_correlation(const std::string& sym1, const std::string& sym2, int bars) const;

    // Returns (price, bar_count) of bars
    std::vector<double> get_close_series(const std::string& symbol, int count) const;

    // Callback when a new bar completes
    void set_bar_callback(std::function<void(const std::string&, const BarData&)> cb);

private:
    int aggregation_ms_;
    int max_history_bars_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, MarketState> states_;
    std::function<void(const std::string&, const BarData&)> bar_callback_;

    void complete_bar(const std::string& symbol);
    void update_vwap(MarketState& state, double price, int size);
};

} // namespace shieldorder
