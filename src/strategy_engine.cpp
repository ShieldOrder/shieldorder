#include "strategy_engine.h"
#include "logger.h"
#include <cmath>
#include <numeric>
#include <algorithm>
#include <chrono>

namespace shieldorder {

StrategyEngine::StrategyEngine(const StrategyConfig& config)
    : config_(config) {}

StrategyOutput StrategyEngine::evaluate(const MarketDataHandler& market_data) {
    StrategyOutput output;
    output.state = state_;

    if (state_ == StrategyState::HALTED) {
        return output;
    }

    // Check trading hours
    if (!is_trading_hours()) {
        output.state = StrategyState::IDLE;
        return output;
    }

    // Need data for all instruments
    if (!market_data.has_data(config_.mini_dax_symbol) ||
        !market_data.has_data(config_.eurostoxx_symbol)) {
        return output;
    }

    // Compute DAX/Stoxx correlation
    output.dax_stoxx_correlation = market_data.compute_correlation(
        config_.mini_dax_symbol, config_.eurostoxx_symbol, config_.lookback_bars);

    // Generate signals
    Signal dax_signal = generate_dax_signal(market_data);
    Signal stoxx_signal = generate_stoxx_signal(market_data);

    output.dax_momentum = dax_signal.strength;
    output.stoxx_momentum = stoxx_signal.strength;

    // DAX signal: enter if momentum exceeds threshold
    if (dax_signal.strength >= config_.momentum_threshold) {
        if (spread_acceptable(market_data, config_.mini_dax_symbol)) {
            output.signals.push_back(dax_signal);
            state_ = StrategyState::ACTIVE;
        }
    }

    // Stoxx signal: enter if correlated and has its own momentum
    if (stoxx_signal.strength >= config_.momentum_threshold &&
        output.dax_stoxx_correlation >= config_.correlation_threshold) {
        if (spread_acceptable(market_data, config_.eurostoxx_symbol)) {
            output.signals.push_back(stoxx_signal);
        }
    }

    // If DAX signal is very strong, suggest scaling
    if (dax_signal.strength >= config_.scale_threshold) {
        state_ = StrategyState::SCALING;
    }

    output.state = state_;
    return output;
}

void StrategyEngine::halt(const std::string& reason) {
    state_ = StrategyState::HALTED;
    LOG_WARN("Strategy", "HALTED: " + reason);
}

void StrategyEngine::resume() {
    state_ = StrategyState::IDLE;
    LOG_INFO("Strategy", "Resumed from halt");
}

Signal StrategyEngine::generate_dax_signal(const MarketDataHandler& md) {
    Signal sig;
    sig.symbol = config_.mini_dax_symbol;
    sig.generated_at = std::chrono::steady_clock::now();

    auto prices = md.get_close_series(config_.mini_dax_symbol, config_.lookback_bars);
    if ((int)prices.size() < config_.lookback_bars) {
        sig.strength = 0.0;
        return sig;
    }

    // Multi-factor signal
    double momentum = compute_momentum(prices, config_.lookback_bars);
    double rsi = compute_rsi(prices, 14);
    double ema_fast = compute_ema(prices, 5);
    double ema_slow = compute_ema(prices, 20);

    const auto& state = md.get_state(config_.mini_dax_symbol);
    int depth_imb = state.depth_imbalance;
    double current_price = prices.back();

    // Determine direction and strength
    double direction_score = 0.0;

    // EMA crossover component (0.0 to 0.4)
    double ema_diff = (ema_fast - ema_slow) / ema_slow;
    direction_score += std::clamp(ema_diff * 100.0, -0.4, 0.4);

    // Momentum component (0.0 to 0.3)
    direction_score += std::clamp(momentum * 0.3, -0.3, 0.3);

    // Depth imbalance component (0.0 to 0.2)
    double imb_normalized = std::clamp(depth_imb / 100.0, -0.2, 0.2);
    direction_score += imb_normalized;

    // RSI component - favor entries in non-extreme zones (0.0 to 0.1)
    if (rsi > 30 && rsi < 70) {
        if (direction_score > 0 && rsi < 60) direction_score += 0.1;
        if (direction_score < 0 && rsi > 40) direction_score -= 0.1;
    }

    // Set signal
    sig.strength = std::abs(direction_score);
    sig.side = (direction_score > 0) ? Side::BUY : Side::SELL;

    // Set stop and target based on ATR
    double atr = compute_atr(md, config_.mini_dax_symbol, 14);
    if (atr == 0.0) atr = config_.trailing_stop_points;  // fallback

    if (sig.side == Side::BUY) {
        sig.target_price = current_price + config_.profit_target_points;
        sig.stop_price = current_price - std::min(config_.trailing_stop_points, atr * 1.5);
    } else {
        sig.target_price = current_price - config_.profit_target_points;
        sig.stop_price = current_price + std::min(config_.trailing_stop_points, atr * 1.5);
    }

    return sig;
}

Signal StrategyEngine::generate_stoxx_signal(const MarketDataHandler& md) {
    Signal sig;
    sig.symbol = config_.eurostoxx_symbol;
    sig.generated_at = std::chrono::steady_clock::now();

    auto prices = md.get_close_series(config_.eurostoxx_symbol, config_.lookback_bars);
    if ((int)prices.size() < config_.lookback_bars) {
        sig.strength = 0.0;
        return sig;
    }

    double momentum = compute_momentum(prices, config_.lookback_bars);
    double ema_fast = compute_ema(prices, 5);
    double ema_slow = compute_ema(prices, 20);

    const auto& state = md.get_state(config_.eurostoxx_symbol);
    double current_price = prices.back();

    // Simpler signal for parallel instrument
    double direction_score = 0.0;
    double ema_diff = (ema_fast - ema_slow) / ema_slow;
    direction_score += std::clamp(ema_diff * 100.0, -0.5, 0.5);
    direction_score += std::clamp(momentum * 0.5, -0.5, 0.5);

    sig.strength = std::abs(direction_score);
    sig.side = (direction_score > 0) ? Side::BUY : Side::SELL;

    if (sig.side == Side::BUY) {
        sig.target_price = current_price + config_.profit_target_points;
        sig.stop_price = current_price - config_.trailing_stop_points;
    } else {
        sig.target_price = current_price - config_.profit_target_points;
        sig.stop_price = current_price + config_.trailing_stop_points;
    }

    return sig;
}

double StrategyEngine::compute_momentum(const std::vector<double>& prices, int period) const {
    if ((int)prices.size() < period) return 0.0;
    double current = prices.back();
    double past = prices[prices.size() - period];
    return (current - past) / past;
}

double StrategyEngine::compute_rsi(const std::vector<double>& prices, int period) const {
    if ((int)prices.size() < period + 1) return 50.0;

    double gain = 0.0, loss = 0.0;
    int start = prices.size() - period - 1;
    for (int i = start + 1; i < (int)prices.size(); ++i) {
        double change = prices[i] - prices[i-1];
        if (change > 0) gain += change;
        else loss -= change;
    }

    gain /= period;
    loss /= period;

    if (loss == 0.0) return 100.0;
    double rs = gain / loss;
    return 100.0 - (100.0 / (1.0 + rs));
}

double StrategyEngine::compute_ema(const std::vector<double>& prices, int period) const {
    if (prices.empty()) return 0.0;
    if ((int)prices.size() < period) {
        return std::accumulate(prices.begin(), prices.end(), 0.0) / prices.size();
    }

    double multiplier = 2.0 / (period + 1);
    double ema = prices[prices.size() - period];  // Seed with first value in window

    for (int i = prices.size() - period + 1; i < (int)prices.size(); ++i) {
        ema = (prices[i] - ema) * multiplier + ema;
    }
    return ema;
}

double StrategyEngine::compute_atr(const MarketDataHandler& md, const std::string& symbol, int period) const {
    auto series = md.get_close_series(symbol, period + 1);
    if ((int)series.size() < 2) return 0.0;

    // Simplified ATR using close-to-close (proper ATR needs H/L/C bars)
    double sum = 0.0;
    for (size_t i = 1; i < series.size(); ++i) {
        sum += std::abs(series[i] - series[i-1]);
    }
    return sum / (series.size() - 1);
}

double StrategyEngine::compute_spread_zscore(const MarketDataHandler& md) const {
    auto dax_prices = md.get_close_series(config_.mini_dax_symbol, config_.lookback_bars);
    auto stoxx_prices = md.get_close_series(config_.eurostoxx_symbol, config_.lookback_bars);

    int n = std::min(dax_prices.size(), stoxx_prices.size());
    if (n < 10) return 0.0;

    // Compute spread ratio
    std::vector<double> spreads(n);
    for (int i = 0; i < n; ++i) {
        spreads[i] = dax_prices[i] / stoxx_prices[i];
    }

    double mean = std::accumulate(spreads.begin(), spreads.end(), 0.0) / n;
    double sq_sum = 0.0;
    for (double s : spreads) {
        sq_sum += (s - mean) * (s - mean);
    }
    double stdev = std::sqrt(sq_sum / n);

    if (stdev == 0.0) return 0.0;
    return (spreads.back() - mean) / stdev;
}

bool StrategyEngine::is_correlation_valid(const MarketDataHandler& md) const {
    double corr = md.compute_correlation(config_.mini_dax_symbol, config_.eurostoxx_symbol,
                                         config_.lookback_bars);
    return corr >= config_.correlation_threshold;
}

bool StrategyEngine::is_trading_hours() const {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
    gmtime_r(&time_t, &tm_buf);

    int hour = tm_buf.tm_hour;
    return (hour >= config_.start_hour_utc && hour < config_.end_hour_utc);
}

bool StrategyEngine::spread_acceptable(const MarketDataHandler& md, const std::string& symbol) const {
    double spread = md.get_spread(symbol);
    // Compare spread to instrument tick size (simplified)
    return spread <= config_.max_spread_ticks;
}

} // namespace shieldorder
