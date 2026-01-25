#pragma once

#include "order_flow_types.h"
#include <deque>
#include <mutex>
#include <vector>

namespace shieldorder {

class MarketStructure {
public:
    struct Config {
        int swing_strength = 3;             // Bars to confirm a swing point
        double liquidity_zone_width = 5.0;  // Points around swing for liquidity
        int fvg_min_size_ticks = 2;         // Minimum gap size to qualify as FVG
        int max_swing_points = 50;
        int max_fvgs = 20;
        int max_liquidity_levels = 20;
        double broken_level_buffer = 2.0;   // Points beyond level to mark as broken
    };

    explicit MarketStructure(const Config& config = Config{});

    // Feed price data
    void on_bar(double open, double high, double low, double close, Timestamp time);

    // Swing points
    const std::deque<SwingPoint>& swing_points() const { return swings_; }
    SwingPoint last_swing_high() const;
    SwingPoint last_swing_low() const;
    bool is_higher_high() const;
    bool is_lower_low() const;
    bool is_higher_low() const;
    bool is_lower_high() const;

    // Trend determination from structure
    enum class Trend { BULLISH, BEARISH, RANGING };
    Trend current_trend() const;

    // Liquidity levels (where stops likely cluster)
    const std::vector<LiquidityLevel>& liquidity_levels() const { return liquidity_; }
    bool is_near_buy_liquidity(double price, double tolerance = 3.0) const;
    bool is_near_sell_liquidity(double price, double tolerance = 3.0) const;

    // Fair Value Gaps (imbalance zones)
    const std::deque<FairValueGap>& fair_value_gaps() const { return fvgs_; }
    bool is_in_bullish_fvg(double price) const;
    bool is_in_bearish_fvg(double price) const;
    FairValueGap nearest_unfilled_fvg(double price) const;

    // Key levels
    double nearest_support(double price) const;
    double nearest_resistance(double price) const;

private:
    Config config_;
    mutable std::mutex mutex_;

    struct PriceBar {
        double open, high, low, close;
        Timestamp time;
    };
    std::deque<PriceBar> bars_;

    std::deque<SwingPoint> swings_;
    std::vector<LiquidityLevel> liquidity_;
    std::deque<FairValueGap> fvgs_;

    void detect_swing_points();
    void update_liquidity_levels();
    void detect_fvgs();
    void check_broken_levels(double current_price);
    void check_filled_fvgs(double high, double low);
};

} // namespace shieldorder
