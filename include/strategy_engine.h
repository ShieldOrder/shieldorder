#pragma once

#include "types.h"
#include "market_data_handler.h"
#include "config.h"
#include <vector>
#include <functional>

namespace shieldorder {

struct StrategyOutput {
    std::vector<Signal> signals;
    StrategyState state;
    double dax_stoxx_correlation;
    double dax_momentum;
    double stoxx_momentum;
};

class StrategyEngine {
public:
    explicit StrategyEngine(const StrategyConfig& config);

    // Called on each new bar completion
    StrategyOutput evaluate(const MarketDataHandler& market_data);

    // State management
    StrategyState get_state() const { return state_; }
    void set_state(StrategyState s) { state_ = s; }
    void halt(const std::string& reason);
    void resume();

private:
    StrategyConfig config_;
    StrategyState state_ = StrategyState::IDLE;

    // Signal generation
    Signal generate_dax_signal(const MarketDataHandler& md);
    Signal generate_stoxx_signal(const MarketDataHandler& md);

    // Indicators
    double compute_momentum(const std::vector<double>& prices, int period) const;
    double compute_rsi(const std::vector<double>& prices, int period) const;
    double compute_ema(const std::vector<double>& prices, int period) const;
    double compute_atr(const MarketDataHandler& md, const std::string& symbol, int period) const;

    // Spread/correlation logic
    double compute_spread_zscore(const MarketDataHandler& md) const;
    bool is_correlation_valid(const MarketDataHandler& md) const;

    // Filters
    bool is_trading_hours() const;
    bool spread_acceptable(const MarketDataHandler& md, const std::string& symbol) const;
};

} // namespace shieldorder
