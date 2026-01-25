#pragma once

#include <string>
#include <chrono>
#include <cstdint>

namespace shieldorder {

using Timestamp = std::chrono::steady_clock::time_point;
using SystemClock = std::chrono::system_clock::time_point;

enum class Side { BUY, SELL };
enum class OrderType { MARKET, LIMIT, STOP, STOP_LIMIT };
enum class OrderState { PENDING, SENT, ACKNOWLEDGED, PARTIAL_FILL, FILLED, CANCELLED, REJECTED };
enum class StrategyState { IDLE, ACTIVE, SCALING, CLOSING, HALTED };

struct Instrument {
    std::string symbol;       // e.g., "FDXS", "FDAX", "FESX"
    std::string exchange;     // "EUREX"
    double tick_size;         // Minimum price increment
    double tick_value;        // Value per tick in EUR
    int contract_multiplier;  // Point value
};

struct Tick {
    std::string symbol;
    double bid;
    double ask;
    double last;
    int bid_size;
    int ask_size;
    int last_size;
    uint64_t volume;
    Timestamp local_time;
    SystemClock exchange_time;
};

struct DepthLevel {
    double price;
    int size;
    int order_count;
};

struct DepthUpdate {
    std::string symbol;
    std::vector<DepthLevel> bids;  // sorted best to worst
    std::vector<DepthLevel> asks;  // sorted best to worst
    Timestamp local_time;
};

struct Order {
    std::string order_id;
    std::string symbol;
    Side side;
    OrderType type;
    double price;
    int quantity;
    int filled_quantity;
    OrderState state;
    Timestamp submit_time;
    std::string rithmic_order_id;
};

struct Position {
    std::string symbol;
    int net_quantity;        // positive = long, negative = short
    double avg_entry_price;
    double unrealized_pnl;
    double realized_pnl;
};

struct Signal {
    std::string symbol;
    Side side;
    double strength;        // 0.0 to 1.0
    double target_price;
    double stop_price;
    Timestamp generated_at;
};

// Eurex instrument definitions
inline Instrument FDXM() {
    return {"FDXM", "EUREX", 1.0, 5.0, 5};    // Mini DAX: 5 EUR per point, 1pt ticks
}

inline Instrument FDAX() {
    return {"FDAX", "EUREX", 0.5, 12.50, 25};  // DAX: 25 EUR per point, 0.5pt ticks
}

inline Instrument FESX() {
    return {"FESX", "EUREX", 1.0, 10.0, 10};  // Euro Stoxx 50: 10 EUR per point
}

} // namespace shieldorder
