#pragma once

#include "types.h"
#include <vector>
#include <deque>
#include <unordered_map>
#include <map>
#include <cmath>

namespace shieldorder {

// ============================================================================
// TRADE CLASSIFICATION
// ============================================================================

enum class TradeAggressor { BUYER, SELLER, UNKNOWN };
enum class TradeSize { SMALL, MEDIUM, LARGE, INSTITUTIONAL };

struct ClassifiedTrade {
    double price;
    int size;
    TradeAggressor aggressor;
    TradeSize size_class;
    Timestamp time;
    uint64_t sequence;          // Monotonic sequence for ordering

    double bid_at_trade;        // BBO at time of trade
    double ask_at_trade;
    int bid_size_at_trade;
    int ask_size_at_trade;
};

// ============================================================================
// FOOTPRINT / DELTA AT PRICE
// ============================================================================

struct FootprintLevel {
    double price;
    int buy_volume;             // Aggressive buying at this price
    int sell_volume;            // Aggressive selling at this price
    int delta;                  // buy_volume - sell_volume
    int total_volume;
    int trade_count;
    int max_single_trade;       // Largest single trade at this level

    double imbalance_ratio() const {
        if (sell_volume == 0) return (buy_volume > 0) ? 10.0 : 0.0;
        return (double)buy_volume / sell_volume;
    }

    bool is_buy_imbalance(double threshold = 3.0) const {
        return imbalance_ratio() >= threshold;
    }

    bool is_sell_imbalance(double threshold = 3.0) const {
        if (buy_volume == 0) return sell_volume > 0;
        return ((double)sell_volume / buy_volume) >= threshold;
    }
};

struct FootprintBar {
    Timestamp start_time;
    Timestamp end_time;
    double open, high, low, close;
    int total_volume;
    int delta;                  // Net delta for entire bar
    int max_delta;              // Peak delta during bar
    int min_delta;              // Trough delta during bar
    std::map<double, FootprintLevel> levels;  // Price -> footprint data

    // Diagonal imbalance detection (stacked buy/sell imbalance)
    int count_stacked_buy_imbalance(double threshold = 3.0) const;
    int count_stacked_sell_imbalance(double threshold = 3.0) const;

    // Point of Control for this bar
    double poc() const;

    // Delta divergence: price made new high but delta didn't (or vice versa)
    bool has_delta_divergence(const FootprintBar& prev) const;
};

// ============================================================================
// VOLUME PROFILE
// ============================================================================

struct VolumeProfileLevel {
    double price;
    int volume;
    int buy_volume;
    int sell_volume;
    int delta;
    int trade_count;
};

struct VolumeProfile {
    std::map<double, VolumeProfileLevel> levels;
    double poc_price = 0.0;             // Point of Control (highest volume price)
    double value_area_high = 0.0;       // 70% of volume above this
    double value_area_low = 0.0;        // 70% of volume below this
    int total_volume = 0;
    double tick_size = 1.0;

    void add_trade(double price, int volume, TradeAggressor aggressor);
    void compute_value_area(double pct = 0.70);
    bool is_in_value_area(double price) const;
    bool is_at_poc(double price, double tolerance = 2.0) const;
    bool is_single_print(double price) const;  // Low volume = potential support/resistance
    std::vector<double> find_high_volume_nodes(double min_pct = 0.05) const;
    std::vector<double> find_low_volume_nodes(double max_pct = 0.01) const;
};

// ============================================================================
// ORDER BOOK ANALYTICS
// ============================================================================

struct BookLevel {
    double price;
    int size;
    int order_count;
    Timestamp last_update;
    int size_delta;             // Change since last update (positive = added, negative = pulled)
};

struct BookSnapshot {
    std::vector<BookLevel> bids;
    std::vector<BookLevel> asks;
    Timestamp time;

    double best_bid() const { return bids.empty() ? 0.0 : bids[0].price; }
    double best_ask() const { return asks.empty() ? 0.0 : asks[0].price; }
    double mid_price() const { return (best_bid() + best_ask()) / 2.0; }
    double spread() const { return best_ask() - best_bid(); }
    int bid_depth(int levels = 5) const;
    int ask_depth(int levels = 5) const;
    double imbalance_ratio(int levels = 5) const;  // bid_depth / ask_depth
};

enum class BookEvent {
    NONE,
    BID_STACK,          // Large size added to bids (support building)
    BID_PULL,           // Size removed from bids (support weakening)
    ASK_STACK,          // Large size added to asks (resistance building)
    ASK_PULL,           // Size removed from asks (resistance weakening)
    ABSORPTION_BID,     // Bid being hit repeatedly but not moving (absorption)
    ABSORPTION_ASK,     // Ask being lifted repeatedly but not moving
    ICEBERG_BID,        // Hidden size detected on bid (refills after trades)
    ICEBERG_ASK,        // Hidden size detected on ask
    SWEEP_BID,          // Multiple bid levels taken out rapidly
    SWEEP_ASK,          // Multiple ask levels taken out rapidly
    FLIP,               // Book flipped from bid-heavy to ask-heavy or vice versa
};

struct BookEventRecord {
    BookEvent event;
    double price;
    int size;
    Timestamp time;
    double significance;  // 0.0 to 1.0, how significant this event is
};

// ============================================================================
// MARKET STRUCTURE
// ============================================================================

enum class SwingType { SWING_HIGH, SWING_LOW };

struct SwingPoint {
    double price;
    Timestamp time;
    SwingType type;
    int strength;           // How many bars confirmed this swing
    bool broken = false;    // Whether price has traded through this level
};

struct LiquidityLevel {
    double price;
    double strength;        // How strong the liquidity pool is
    bool is_buy_side;       // Buy stops above (or sell stops below)
    int times_tested;       // How many times price approached
    Timestamp first_seen;
};

struct FairValueGap {
    double high;            // Top of gap
    double low;             // Bottom of gap
    bool is_bullish;        // Bullish FVG = gap up (buy zone)
    Timestamp time;
    bool filled = false;
};

// ============================================================================
// TAPE SPEED / MOMENTUM
// ============================================================================

struct TapeMetrics {
    double trades_per_second;
    double volume_per_second;
    double avg_trade_size;
    int large_trade_count;          // Trades > 2x average in last N seconds
    double buy_pressure;            // 0.0 to 1.0 (% aggressive buys)
    double sell_pressure;           // 0.0 to 1.0 (% aggressive sells)
    double velocity;                // Price change per second
    double acceleration;            // Change in velocity
    int consecutive_buy_ticks;      // Unbroken run of upticks
    int consecutive_sell_ticks;     // Unbroken run of downticks

    bool is_climactic_buying() const {
        return trades_per_second > 20.0 && buy_pressure > 0.75 && large_trade_count > 3;
    }
    bool is_climactic_selling() const {
        return trades_per_second > 20.0 && sell_pressure > 0.75 && large_trade_count > 3;
    }
    bool is_exhaustion_buying() const {
        // High activity but price not advancing - buyers absorbing but losing momentum
        return trades_per_second > 15.0 && buy_pressure > 0.65 && velocity < 0.5;
    }
    bool is_exhaustion_selling() const {
        return trades_per_second > 15.0 && sell_pressure > 0.65 && velocity > -0.5;
    }
};

// ============================================================================
// CONFLUENCE SIGNAL (what the scalper uses to decide)
// ============================================================================

struct ScalpSignal {
    Side side;
    double confidence;          // 0.0 to 1.0, combined confluence score
    double entry_price;
    double stop_price;
    double target_price;
    Timestamp generated_at;
    std::string trigger_reason;

    // Individual factor scores that contributed
    double order_flow_score;    // Delta / footprint analysis
    double book_score;          // Order book dynamics
    double structure_score;     // Market structure (levels, FVGs)
    double tape_score;          // Tape speed / momentum
    double volume_score;        // Volume profile context
};

} // namespace shieldorder
