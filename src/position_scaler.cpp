#include "position_scaler.h"
#include "logger.h"
#include <cmath>

namespace shieldorder {

PositionScaler::PositionScaler(const StrategyConfig& config)
    : config_(config) {}

ScaleDecision PositionScaler::evaluate(const Signal& signal,
                                       const std::unordered_map<std::string, Position>& positions,
                                       double current_pnl) {
    ScaleDecision decision;
    decision.action = ScaleAction::NONE;
    decision.side = signal.side;

    // Check if we have existing positions
    auto mini_it = positions.find(config_.mini_dax_symbol);
    auto full_it = positions.find(config_.dax_symbol);
    auto stoxx_it = positions.find(config_.eurostoxx_symbol);

    bool has_mini = (mini_it != positions.end() && mini_it->second.net_quantity != 0);
    bool has_full = (full_it != positions.end() && full_it->second.net_quantity != 0);
    bool has_stoxx = (stoxx_it != positions.end() && stoxx_it->second.net_quantity != 0);

    // --- DAX signal logic ---
    if (signal.symbol == config_.mini_dax_symbol || signal.symbol == config_.dax_symbol) {

        if (!has_mini && !has_full) {
            // No position: enter with mini DAX
            decision.action = ScaleAction::ENTER_MINI;
            decision.symbol = config_.mini_dax_symbol;
            decision.quantity = calculate_size(config_.mini_dax_symbol, signal.strength);
            decision.price = signal.target_price;  // Will use limit near market
            decision.reason = "New FDXM entry, signal=" + std::to_string(signal.strength);
            return decision;
        }

        if (has_mini && !has_full) {
            // Have mini position: check if we should scale up
            if (should_scale_up(signal, mini_it->second, mini_it->second.unrealized_pnl)) {
                decision.action = ScaleAction::SCALE_TO_FULL;
                decision.symbol = config_.dax_symbol;
                decision.quantity = calculate_size(config_.dax_symbol, signal.strength);
                decision.price = signal.target_price;
                decision.reason = "Scaling FDXM->FDAX, signal=" + std::to_string(signal.strength);
                return decision;
            }

            // Or add to mini position
            if (!at_max_position(config_.mini_dax_symbol, std::abs(mini_it->second.net_quantity))) {
                decision.action = ScaleAction::ADD_MINI;
                decision.symbol = config_.mini_dax_symbol;
                decision.quantity = 1;
                decision.price = signal.target_price;
                decision.reason = "Adding to FDXM position";
                return decision;
            }
        }

        if (has_full) {
            // Already have full DAX position: add if room
            if (!at_max_position(config_.dax_symbol, std::abs(full_it->second.net_quantity))) {
                decision.action = ScaleAction::ADD_FULL;
                decision.symbol = config_.dax_symbol;
                decision.quantity = 1;
                decision.price = signal.target_price;
                decision.reason = "Adding to FDAX position";
                return decision;
            }
        }
    }

    // --- FESX signal logic ---
    if (signal.symbol == config_.eurostoxx_symbol) {
        if (!has_stoxx) {
            // Check if we should enter Stoxx alongside DAX
            if (has_mini || has_full) {
                const Position& dax_pos = has_full ? full_it->second : mini_it->second;
                if (should_enter_stoxx(signal, dax_pos)) {
                    decision.action = ScaleAction::ENTER_STOXX;
                    decision.symbol = config_.eurostoxx_symbol;
                    decision.quantity = calculate_size(config_.eurostoxx_symbol, signal.strength);
                    decision.price = signal.target_price;
                    decision.reason = "Parallel FESX entry alongside DAX";
                    return decision;
                }
            }
        }
    }

    return decision;
}

bool PositionScaler::should_scale_up(const Signal& signal,
                                     const Position& mini_pos,
                                     double unrealized_pnl) const {
    // Scale to full DAX when:
    // 1. Signal strength exceeds scale threshold
    // 2. Current mini position is profitable (confirms direction)
    // 3. Position is in same direction as signal

    if (signal.strength < config_.scale_threshold) return false;
    if (unrealized_pnl <= 0.0) return false;

    bool same_direction = (signal.side == Side::BUY && mini_pos.net_quantity > 0) ||
                          (signal.side == Side::SELL && mini_pos.net_quantity < 0);
    if (!same_direction) return false;

    LOG_INFO("Scaler", "Scale-up conditions met: strength=" + std::to_string(signal.strength)
             + " unrealized_pnl=" + std::to_string(unrealized_pnl));
    return true;
}

bool PositionScaler::should_enter_stoxx(const Signal& stoxx_signal,
                                        const Position& dax_pos) const {
    // Enter FESX when:
    // 1. DAX position exists and is in same direction
    // 2. Stoxx signal confirms the direction
    // 3. Signal strength is adequate

    if (stoxx_signal.strength < config_.momentum_threshold) return false;

    bool same_direction = (stoxx_signal.side == Side::BUY && dax_pos.net_quantity > 0) ||
                          (stoxx_signal.side == Side::SELL && dax_pos.net_quantity < 0);

    return same_direction;
}

int PositionScaler::calculate_size(const std::string& symbol, double signal_strength) const {
    if (symbol == config_.mini_dax_symbol) {
        // Scale between 1 and initial_mini_lots based on signal strength
        int base = config_.initial_mini_lots;
        int size = std::max(1, (int)(base * signal_strength / config_.momentum_threshold));
        return std::min(size, config_.max_fdxm_lots);
    }
    else if (symbol == config_.dax_symbol) {
        // Full DAX: more conservative, 1 at a time
        return 1;
    }
    else if (symbol == config_.eurostoxx_symbol) {
        // Stoxx: scale based on signal but cap
        int size = std::max(1, (int)(2 * signal_strength));
        return std::min(size, config_.max_fesx_lots);
    }

    return 1;
}

bool PositionScaler::at_max_position(const std::string& symbol, int current_qty) const {
    if (symbol == config_.mini_dax_symbol) return current_qty >= config_.max_fdxm_lots;
    if (symbol == config_.dax_symbol) return current_qty >= config_.max_fdax_lots;
    if (symbol == config_.eurostoxx_symbol) return current_qty >= config_.max_fesx_lots;
    return true;
}

} // namespace shieldorder
