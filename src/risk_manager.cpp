#include "risk_manager.h"
#include "logger.h"
#include <cmath>

namespace shieldorder {

RiskManager::RiskManager(const StrategyConfig& config)
    : config_(config) {}

RiskCheckResult RiskManager::check_order(const std::string& symbol, Side side,
                                         int quantity, double price) {
    if (status_.is_halted) {
        LOG_WARN("RiskMgr", "Order rejected: strategy halted - " + status_.halt_reason);
        return RiskCheckResult::REJECTED;
    }

    // Check daily loss limit
    if (check_daily_loss_limit()) {
        halt("Daily loss limit reached: " + std::to_string(status_.daily_pnl) + " EUR");
        return RiskCheckResult::HALT_STRATEGY;
    }

    // Check position limits
    if (!check_position_limit(symbol, quantity)) {
        LOG_WARN("RiskMgr", "Order rejected: position limit for " + symbol);
        return RiskCheckResult::REJECTED;
    }

    // Check max open orders
    // (caller should pass current open order count)
    if (!check_order_count(config_.max_open_orders - 1)) {
        LOG_WARN("RiskMgr", "Order rejected: max open orders reached");
        return RiskCheckResult::REDUCE_SIZE;
    }

    // Check consecutive losses (reduce size after 3 consecutive losses)
    if (status_.consecutive_losses >= 3) {
        LOG_WARN("RiskMgr", "Reducing size due to " +
                 std::to_string(status_.consecutive_losses) + " consecutive losses");
        return RiskCheckResult::REDUCE_SIZE;
    }

    return RiskCheckResult::APPROVED;
}

RiskCheckResult RiskManager::check_position_risk(const Position& pos, double current_price) {
    if (pos.net_quantity == 0) return RiskCheckResult::APPROVED;

    // Check per-position loss limit
    if (pos.unrealized_pnl < -config_.max_position_loss_eur) {
        LOG_WARN("RiskMgr", "Position loss limit hit for " + pos.symbol +
                 ": " + std::to_string(pos.unrealized_pnl) + " EUR");
        return RiskCheckResult::HALT_STRATEGY;
    }

    // Check trailing stop
    Side pos_side = (pos.net_quantity > 0) ? Side::BUY : Side::SELL;
    double stop = get_trailing_stop(pos.symbol, pos_side);

    if (stop != 0.0) {
        if (pos_side == Side::BUY && current_price <= stop) {
            LOG_WARN("RiskMgr", "Trailing stop hit for LONG " + pos.symbol +
                     " @ " + std::to_string(current_price));
            return RiskCheckResult::HALT_STRATEGY;
        }
        if (pos_side == Side::SELL && current_price >= stop) {
            LOG_WARN("RiskMgr", "Trailing stop hit for SHORT " + pos.symbol +
                     " @ " + std::to_string(current_price));
            return RiskCheckResult::HALT_STRATEGY;
        }
    }

    return RiskCheckResult::APPROVED;
}

void RiskManager::on_fill(const std::string& symbol, Side side, int qty, double price) {
    status_.total_trades++;
    LOG_INFO("RiskMgr", "Fill recorded: " + symbol + " " +
             (side == Side::BUY ? "BUY" : "SELL") + " " +
             std::to_string(qty) + " @ " + std::to_string(price));
}

void RiskManager::on_position_update(const Position& pos) {
    // Update daily P&L
    double total_pnl = pos.realized_pnl + pos.unrealized_pnl;

    // Track peak and drawdown
    if (total_pnl > status_.peak_pnl) {
        status_.peak_pnl = total_pnl;
    }
    status_.max_drawdown = std::min(status_.max_drawdown, total_pnl - status_.peak_pnl);

    // Track consecutive losses on realized P&L changes
    if (pos.realized_pnl < 0) {
        status_.consecutive_losses++;
    } else if (pos.realized_pnl > 0) {
        status_.consecutive_losses = 0;
    }

    status_.daily_pnl = total_pnl;

    // Update trailing stop
    if (pos.net_quantity != 0) {
        Side side = (pos.net_quantity > 0) ? Side::BUY : Side::SELL;
        // Use last tick price approximation from avg_entry + unrealized
        double approx_current = pos.avg_entry_price;
        if (pos.net_quantity != 0) {
            // unrealized_pnl = (current - entry) * qty * multiplier (simplified)
            approx_current = pos.avg_entry_price + (pos.unrealized_pnl / std::abs(pos.net_quantity));
        }
        update_trailing_stop(pos.symbol, approx_current, side);
    }
}

void RiskManager::reset_daily() {
    LOG_INFO("RiskMgr", "Daily reset - Previous P&L: " + std::to_string(status_.daily_pnl) +
             " EUR, Trades: " + std::to_string(status_.total_trades));

    status_.daily_pnl = 0.0;
    status_.peak_pnl = 0.0;
    status_.max_drawdown = 0.0;
    status_.total_trades = 0;
    status_.consecutive_losses = 0;
    status_.is_halted = false;
    status_.halt_reason.clear();
    trailing_stops_.clear();
    high_water_marks_.clear();
}

double RiskManager::get_trailing_stop(const std::string& symbol, Side side) const {
    auto it = trailing_stops_.find(symbol);
    return (it != trailing_stops_.end()) ? it->second : 0.0;
}

void RiskManager::update_trailing_stop(const std::string& symbol, double current_price, Side side) {
    auto& hwm = high_water_marks_[symbol];

    if (side == Side::BUY) {
        // Long: trailing stop moves up, never down
        if (current_price > hwm || hwm == 0.0) {
            hwm = current_price;
            trailing_stops_[symbol] = hwm - config_.trailing_stop_points;
        }
    } else {
        // Short: trailing stop moves down, never up
        if (current_price < hwm || hwm == 0.0) {
            hwm = current_price;
            trailing_stops_[symbol] = hwm + config_.trailing_stop_points;
        }
    }
}

void RiskManager::halt(const std::string& reason) {
    status_.is_halted = true;
    status_.halt_reason = reason;
    LOG_ERROR("RiskMgr", "STRATEGY HALTED: " + reason);
}

void RiskManager::resume() {
    status_.is_halted = false;
    status_.halt_reason.clear();
    LOG_INFO("RiskMgr", "Strategy resumed from halt");
}

bool RiskManager::check_daily_loss_limit() const {
    return status_.daily_pnl <= -config_.max_daily_loss_eur;
}

bool RiskManager::check_position_limit(const std::string& symbol, int additional_qty) const {
    // This is a simplified check - in production, query actual positions
    if (symbol == config_.mini_dax_symbol) return additional_qty <= config_.max_fdxm_lots;
    if (symbol == config_.dax_symbol) return additional_qty <= config_.max_fdax_lots;
    if (symbol == config_.eurostoxx_symbol) return additional_qty <= config_.max_fesx_lots;
    return false;
}

bool RiskManager::check_order_count(int current_open) const {
    return current_open < config_.max_open_orders;
}

} // namespace shieldorder
