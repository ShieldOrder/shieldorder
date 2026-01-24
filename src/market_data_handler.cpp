#include "market_data_handler.h"
#include "logger.h"
#include <cmath>
#include <numeric>
#include <algorithm>

namespace shieldorder {

MarketDataHandler::MarketDataHandler(int aggregation_ms, int max_history_bars)
    : aggregation_ms_(aggregation_ms), max_history_bars_(max_history_bars) {}

void MarketDataHandler::on_tick(const Tick& tick) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto& state = states_[tick.symbol];
    state.last_tick = tick;
    state.session_volume += tick.last_size;

    // Update cumulative delta (approximate: uptick = buy, downtick = sell)
    if (tick.last >= tick.ask) {
        state.cumulative_delta += tick.last_size;  // Aggressive buy
    } else if (tick.last <= tick.bid) {
        state.cumulative_delta -= tick.last_size;  // Aggressive sell
    }

    // Update spread average (exponential moving average)
    double spread = tick.ask - tick.bid;
    if (state.spread_avg == 0.0) {
        state.spread_avg = spread;
    } else {
        state.spread_avg = 0.95 * state.spread_avg + 0.05 * spread;
    }

    // Bar aggregation
    auto& bar = state.current_bar;
    if (bar.tick_count == 0) {
        // New bar
        bar.open = bar.high = bar.low = bar.close = tick.last;
        bar.volume = tick.last_size;
        bar.tick_count = 1;
        bar.start_time = tick.local_time;
    } else {
        bar.high = std::max(bar.high, tick.last);
        bar.low = std::min(bar.low, tick.last);
        bar.close = tick.last;
        bar.volume += tick.last_size;
        bar.tick_count++;
    }

    // Check if bar period elapsed
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        tick.local_time - bar.start_time).count();

    if (elapsed >= aggregation_ms_) {
        bar.end_time = tick.local_time;
        complete_bar(tick.symbol);
    }

    // Update VWAP
    update_vwap(state, tick.last, tick.last_size);
}

void MarketDataHandler::on_depth(const DepthUpdate& update) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto& state = states_[update.symbol];

    // Calculate depth imbalance (sum of bid sizes vs ask sizes at top N levels)
    int bid_depth = 0, ask_depth = 0;
    int levels = std::min(5, (int)std::min(update.bids.size(), update.asks.size()));

    for (int i = 0; i < levels; ++i) {
        if (i < (int)update.bids.size()) bid_depth += update.bids[i].size;
        if (i < (int)update.asks.size()) ask_depth += update.asks[i].size;
    }

    state.depth_imbalance = bid_depth - ask_depth;
}

const MarketState& MarketDataHandler::get_state(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);
    static MarketState empty;
    auto it = states_.find(symbol);
    return (it != states_.end()) ? it->second : empty;
}

double MarketDataHandler::get_mid_price(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = states_.find(symbol);
    if (it == states_.end()) return 0.0;
    const auto& tick = it->second.last_tick;
    return (tick.bid + tick.ask) / 2.0;
}

double MarketDataHandler::get_spread(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = states_.find(symbol);
    if (it == states_.end()) return 0.0;
    return it->second.spread_avg;
}

bool MarketDataHandler::has_data(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = states_.find(symbol);
    return it != states_.end() && it->second.last_tick.last > 0.0;
}

double MarketDataHandler::compute_correlation(const std::string& sym1, const std::string& sym2, int bars) const {
    auto s1 = get_close_series(sym1, bars);
    auto s2 = get_close_series(sym2, bars);

    int n = std::min(s1.size(), s2.size());
    if (n < 5) return 0.0;  // Not enough data

    // Convert to returns
    std::vector<double> r1(n - 1), r2(n - 1);
    for (int i = 1; i < n; ++i) {
        r1[i-1] = (s1[i] - s1[i-1]) / s1[i-1];
        r2[i-1] = (s2[i] - s2[i-1]) / s2[i-1];
    }

    // Pearson correlation on returns
    double mean1 = std::accumulate(r1.begin(), r1.end(), 0.0) / r1.size();
    double mean2 = std::accumulate(r2.begin(), r2.end(), 0.0) / r2.size();

    double cov = 0.0, var1 = 0.0, var2 = 0.0;
    for (size_t i = 0; i < r1.size(); ++i) {
        double d1 = r1[i] - mean1;
        double d2 = r2[i] - mean2;
        cov += d1 * d2;
        var1 += d1 * d1;
        var2 += d2 * d2;
    }

    double denom = std::sqrt(var1 * var2);
    return (denom > 0.0) ? (cov / denom) : 0.0;
}

std::vector<double> MarketDataHandler::get_close_series(const std::string& symbol, int count) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<double> result;

    auto it = states_.find(symbol);
    if (it == states_.end()) return result;

    const auto& history = it->second.bar_history;
    int start = std::max(0, (int)history.size() - count);
    for (int i = start; i < (int)history.size(); ++i) {
        result.push_back(history[i].close);
    }
    return result;
}

void MarketDataHandler::set_bar_callback(std::function<void(const std::string&, const BarData&)> cb) {
    bar_callback_ = cb;
}

void MarketDataHandler::complete_bar(const std::string& symbol) {
    auto& state = states_[symbol];
    auto& bar = state.current_bar;

    // Push completed bar to history
    state.bar_history.push_back(bar);
    if ((int)state.bar_history.size() > max_history_bars_) {
        state.bar_history.pop_front();
    }

    // Notify callback
    if (bar_callback_) {
        bar_callback_(symbol, bar);
    }

    // Reset current bar
    bar = BarData{};
}

void MarketDataHandler::update_vwap(MarketState& state, double price, int size) {
    // Cumulative VWAP
    static thread_local double cum_pv = 0.0;
    static thread_local uint64_t cum_vol = 0;

    cum_pv += price * size;
    cum_vol += size;

    if (cum_vol > 0) {
        state.vwap = cum_pv / cum_vol;
    }
}

} // namespace shieldorder
