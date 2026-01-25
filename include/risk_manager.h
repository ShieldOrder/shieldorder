#pragma once

#include "types.h"
#include "config.h"
#include <unordered_map>
#include <string>

namespace shieldorder {

enum class RiskCheckResult { APPROVED, REDUCE_SIZE, REJECTED, HALT_STRATEGY };

struct RiskStatus {
    double daily_pnl = 0.0;
    double max_drawdown = 0.0;
    double peak_pnl = 0.0;
    int total_trades = 0;
    int consecutive_losses = 0;
    bool is_halted = false;
    std::string halt_reason;
};

class RiskManager {
public:
    explicit RiskManager(const StrategyConfig& config);

    // Pre-trade risk check
    RiskCheckResult check_order(const std::string& symbol, Side side,
                                int quantity, double price);

    // Position-level checks
    RiskCheckResult check_position_risk(const Position& pos, double current_price);

    // Update P&L tracking
    void on_fill(const std::string& symbol, Side side, int qty, double price);
    void on_position_update(const Position& pos);

    // Daily reset
    void reset_daily();

    // Trailing stop management
    double get_trailing_stop(const std::string& symbol, Side side) const;
    void update_trailing_stop(const std::string& symbol, double current_price, Side side);

    // Queries
    const RiskStatus& status() const { return status_; }
    bool is_halted() const { return status_.is_halted; }
    double daily_pnl() const { return status_.daily_pnl; }

    // Force halt/resume
    void halt(const std::string& reason);
    void resume();

private:
    StrategyConfig config_;
    RiskStatus status_;
    std::unordered_map<std::string, double> trailing_stops_;
    std::unordered_map<std::string, double> high_water_marks_;

    bool check_daily_loss_limit() const;
    bool check_position_limit(const std::string& symbol, int additional_qty) const;
    bool check_order_count(int current_open) const;
};

} // namespace shieldorder
