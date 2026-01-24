#pragma once

#include "types.h"
#include "config.h"
#include <unordered_map>

namespace shieldorder {

enum class ScaleAction {
    NONE,
    ENTER_MINI,          // Open position in FDXS
    ADD_MINI,            // Add to FDXS position
    SCALE_TO_FULL,       // Move from FDXS to FDAX
    ADD_FULL,            // Add to FDAX position
    ENTER_STOXX,         // Open position in FESX
    REDUCE,              // Reduce position size
    FLATTEN              // Close everything
};

struct ScaleDecision {
    ScaleAction action;
    std::string symbol;
    Side side;
    int quantity;
    double price;
    std::string reason;
};

class PositionScaler {
public:
    explicit PositionScaler(const StrategyConfig& config);

    // Given current positions and a signal, decide what to do
    ScaleDecision evaluate(const Signal& signal,
                          const std::unordered_map<std::string, Position>& positions,
                          double current_pnl);

    // Check if we should scale from mini to full DAX
    bool should_scale_up(const Signal& signal,
                        const Position& mini_pos,
                        double unrealized_pnl) const;

    // Check if we should enter parallel FESX position
    bool should_enter_stoxx(const Signal& stoxx_signal,
                           const Position& dax_pos) const;

    // Position size for given signal strength and instrument
    int calculate_size(const std::string& symbol, double signal_strength) const;

private:
    StrategyConfig config_;

    bool at_max_position(const std::string& symbol, int current_qty) const;
};

} // namespace shieldorder
