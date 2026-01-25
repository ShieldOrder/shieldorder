#pragma once

#include "order_flow_types.h"
#include <deque>
#include <mutex>
#include <functional>

namespace shieldorder {

class FootprintEngine {
public:
    struct Config {
        int bar_duration_ms = 1000;         // Footprint bar duration (1 second for scalping)
        double imbalance_threshold = 3.0;   // Buy/sell ratio for imbalance
        int stacked_imbalance_min = 3;      // Min consecutive levels for stacked imbalance
        int max_history_bars = 500;
        double tick_size = 1.0;             // Price granularity for footprint levels
    };

    explicit FootprintEngine(const Config& config);

    // Process a classified trade
    void on_trade(const ClassifiedTrade& trade);

    // Get current (incomplete) footprint bar
    const FootprintBar& current_bar() const { return current_bar_; }

    // Get completed bar history
    const std::deque<FootprintBar>& history() const { return history_; }

    // Real-time delta tracking
    int current_bar_delta() const { return current_bar_.delta; }
    int rolling_delta(int bars) const;  // Sum of delta over last N bars
    bool is_delta_divergence() const;   // Price vs delta disagreement

    // Imbalance detection on current bar
    bool has_stacked_buy_imbalance() const;
    bool has_stacked_sell_imbalance() const;
    std::vector<double> get_buy_imbalance_prices() const;
    std::vector<double> get_sell_imbalance_prices() const;

    // Finished/unfinished business (levels with incomplete auctions)
    std::vector<double> get_unfinished_business() const;

    // Delta extremes (potential reversal points)
    bool is_delta_extreme_high(int lookback = 20) const;
    bool is_delta_extreme_low(int lookback = 20) const;

    // Bar completion callback
    void set_bar_callback(std::function<void(const FootprintBar&)> cb);

private:
    Config config_;
    mutable std::mutex mutex_;
    FootprintBar current_bar_;
    std::deque<FootprintBar> history_;
    std::function<void(const FootprintBar&)> bar_callback_;
    bool bar_started_ = false;

    void complete_bar();
    double round_to_tick(double price) const;
};

} // namespace shieldorder
