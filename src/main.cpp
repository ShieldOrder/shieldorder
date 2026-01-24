#include "config.h"
#include "logger.h"
#include "rithmic_connection.h"
#include "market_data_handler.h"
#include "strategy_engine.h"
#include "position_scaler.h"
#include "order_manager.h"
#include "risk_manager.h"
#include "types.h"

#include <iostream>
#include <signal.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <unordered_map>

using namespace shieldorder;

static std::atomic<bool> g_running{true};

void signal_handler(int sig) {
    std::cout << "\nShutdown signal received (" << sig << "), closing positions..." << std::endl;
    g_running = false;
}

class TradingApp {
public:
    TradingApp(const Config& config)
        : config_(config),
          connection_(config.rithmic),
          market_data_(config.strategy.tick_aggregation_ms),
          strategy_(config.strategy),
          scaler_(config.strategy),
          order_mgr_(connection_),
          risk_mgr_(config.strategy) {}

    bool initialize() {
        // Setup logging
        Logger::instance().init("shieldorder.log", LogLevel::INFO);
        LOG_INFO("App", "ShieldOrder Rithmic Trading System starting...");
        LOG_INFO("App", "Instruments: " + config_.strategy.mini_dax_symbol + " -> " +
                 config_.strategy.dax_symbol + " + " + config_.strategy.eurostoxx_symbol);

        // Setup Rithmic callbacks
        RithmicCallbacks cbs;
        cbs.on_tick = [this](const Tick& t) { on_tick(t); };
        cbs.on_depth = [this](const DepthUpdate& d) { on_depth(d); };
        cbs.on_order_update = [this](const Order& o) { on_order_update(o); };
        cbs.on_position_update = [this](const Position& p) { on_position_update(p); };
        cbs.on_error = [this](const std::string& e) { on_error(e); };
        cbs.on_connection_status = [this](bool s) { on_connection_status(s); };
        connection_.set_callbacks(cbs);

        // Setup bar completion callback (triggers strategy evaluation)
        market_data_.set_bar_callback([this](const std::string& sym, const BarData& bar) {
            on_bar_complete(sym, bar);
        });

        // Connect to Rithmic
        if (!connection_.connect()) {
            LOG_ERROR("App", "Failed to connect to Rithmic");
            return false;
        }

        // Subscribe to market data for all instruments
        std::string exchange = config_.strategy.exchange;
        connection_.subscribe_market_data(config_.strategy.mini_dax_symbol, exchange);
        connection_.subscribe_market_data(config_.strategy.dax_symbol, exchange);
        connection_.subscribe_market_data(config_.strategy.eurostoxx_symbol, exchange);

        connection_.subscribe_depth(config_.strategy.mini_dax_symbol, exchange, 5);
        connection_.subscribe_depth(config_.strategy.eurostoxx_symbol, exchange, 5);

        // Request current positions
        connection_.request_positions();

        LOG_INFO("App", "Initialization complete. Waiting for market data...");
        return true;
    }

    void run() {
        LOG_INFO("App", "Main loop started");

        while (g_running) {
            // Main loop: the strategy is event-driven via callbacks
            // This loop handles periodic housekeeping

            std::this_thread::sleep_for(std::chrono::seconds(1));

            // Check if we need to flatten (end of day)
            check_flatten_time();

            // Check daily reset (new session)
            check_daily_reset();
        }

        shutdown();
    }

    void shutdown() {
        LOG_INFO("App", "Shutting down...");

        // Cancel all open orders
        order_mgr_.cancel_all();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Flatten all positions
        flatten_all();
        std::this_thread::sleep_for(std::chrono::seconds(2));

        // Disconnect
        connection_.disconnect();

        LOG_INFO("App", "Shutdown complete. Daily P&L: " +
                 std::to_string(risk_mgr_.daily_pnl()) + " EUR");
    }

private:
    Config config_;
    RithmicConnection connection_;
    MarketDataHandler market_data_;
    StrategyEngine strategy_;
    PositionScaler scaler_;
    OrderManager order_mgr_;
    RiskManager risk_mgr_;

    std::unordered_map<std::string, Position> positions_;
    std::mutex positions_mutex_;

    // --- Event handlers ---

    void on_tick(const Tick& tick) {
        market_data_.on_tick(tick);

        // Real-time risk check on positions
        std::lock_guard<std::mutex> lock(positions_mutex_);
        auto it = positions_.find(tick.symbol);
        if (it != positions_.end() && it->second.net_quantity != 0) {
            double mid = (tick.bid + tick.ask) / 2.0;
            auto risk_result = risk_mgr_.check_position_risk(it->second, mid);
            if (risk_result == RiskCheckResult::HALT_STRATEGY) {
                emergency_flatten(tick.symbol);
            }
        }
    }

    void on_depth(const DepthUpdate& update) {
        market_data_.on_depth(update);
    }

    void on_bar_complete(const std::string& symbol, const BarData& bar) {
        // Only evaluate strategy on DAX bars (primary signal source)
        if (symbol != config_.strategy.mini_dax_symbol &&
            symbol != config_.strategy.eurostoxx_symbol) {
            return;
        }

        // Run strategy evaluation
        auto output = strategy_.evaluate(market_data_);

        // Process each signal
        for (const auto& signal : output.signals) {
            process_signal(signal);
        }
    }

    void process_signal(const Signal& signal) {
        // Risk check first
        auto risk_result = risk_mgr_.check_order(signal.symbol, signal.side,
                                                  1, signal.target_price);

        if (risk_result == RiskCheckResult::REJECTED) {
            LOG_INFO("App", "Signal rejected by risk manager for " + signal.symbol);
            return;
        }
        if (risk_result == RiskCheckResult::HALT_STRATEGY) {
            strategy_.halt("Risk limit breached");
            return;
        }

        // Determine scaling action
        std::lock_guard<std::mutex> lock(positions_mutex_);
        ScaleDecision decision = scaler_.evaluate(signal, positions_, risk_mgr_.daily_pnl());

        if (decision.action == ScaleAction::NONE) return;

        // Reduce size if risk manager says so
        int qty = decision.quantity;
        if (risk_result == RiskCheckResult::REDUCE_SIZE) {
            qty = std::max(1, qty / 2);
        }

        // Check max open orders
        if (order_mgr_.open_order_count() >= config_.strategy.max_open_orders) {
            LOG_WARN("App", "Max open orders reached, skipping signal");
            return;
        }

        // Execute the decision
        execute_decision(decision, qty, signal);
    }

    void execute_decision(const ScaleDecision& decision, int qty, const Signal& signal) {
        std::string exchange = config_.strategy.exchange;

        LOG_INFO("App", "Executing: " + decision.reason + " qty=" + std::to_string(qty));

        switch (decision.action) {
            case ScaleAction::ENTER_MINI:
            case ScaleAction::ADD_MINI:
            case ScaleAction::ENTER_STOXX: {
                // Use limit order near the market
                double limit_price = (signal.side == Side::BUY)
                    ? market_data_.get_mid_price(decision.symbol) + 1.0  // 1 tick aggressive
                    : market_data_.get_mid_price(decision.symbol) - 1.0;

                order_mgr_.send_limit_order(decision.symbol, exchange,
                                            decision.side, qty, limit_price);

                // Protective stop
                Side stop_side = (decision.side == Side::BUY) ? Side::SELL : Side::BUY;
                order_mgr_.send_stop_order(decision.symbol, exchange,
                                           stop_side, qty, signal.stop_price);
                break;
            }

            case ScaleAction::SCALE_TO_FULL:
            case ScaleAction::ADD_FULL: {
                // Scaling to FDAX: use limit order
                double limit_price = (signal.side == Side::BUY)
                    ? market_data_.get_mid_price(config_.strategy.dax_symbol) + 0.5
                    : market_data_.get_mid_price(config_.strategy.dax_symbol) - 0.5;

                order_mgr_.send_limit_order(config_.strategy.dax_symbol, exchange,
                                            decision.side, qty, limit_price);

                // Stop for FDAX
                Side stop_side = (decision.side == Side::BUY) ? Side::SELL : Side::BUY;
                order_mgr_.send_stop_order(config_.strategy.dax_symbol, exchange,
                                           stop_side, qty, signal.stop_price);
                break;
            }

            case ScaleAction::FLATTEN:
                flatten_all();
                break;

            default:
                break;
        }
    }

    void on_order_update(const Order& order) {
        order_mgr_.on_order_update(order);

        if (order.state == OrderState::FILLED) {
            risk_mgr_.on_fill(order.symbol, order.side, order.filled_quantity, order.price);
        }
    }

    void on_position_update(const Position& pos) {
        {
            std::lock_guard<std::mutex> lock(positions_mutex_);
            positions_[pos.symbol] = pos;
        }
        risk_mgr_.on_position_update(pos);
    }

    void on_error(const std::string& error) {
        LOG_ERROR("App", "Rithmic error: " + error);
    }

    void on_connection_status(bool connected) {
        if (connected) {
            LOG_INFO("App", "Rithmic connection established");
        } else {
            LOG_ERROR("App", "Rithmic connection lost");
            strategy_.halt("Connection lost");
        }
    }

    // --- Housekeeping ---

    void check_flatten_time() {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf;
        gmtime_r(&time_t, &tm_buf);

        if (tm_buf.tm_hour >= config_.strategy.flatten_hour_utc) {
            if (strategy_.get_state() != StrategyState::HALTED) {
                LOG_INFO("App", "Flatten time reached, closing all positions");
                flatten_all();
                strategy_.halt("End of day flatten");
            }
        }
    }

    void check_daily_reset() {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf;
        gmtime_r(&time_t, &tm_buf);

        // Reset at start of trading day
        if (tm_buf.tm_hour == config_.strategy.start_hour_utc && tm_buf.tm_min == 0) {
            static int last_reset_day = -1;
            if (tm_buf.tm_mday != last_reset_day) {
                last_reset_day = tm_buf.tm_mday;
                risk_mgr_.reset_daily();
                strategy_.resume();
                LOG_INFO("App", "New trading day - strategy active");
            }
        }
    }

    void flatten_all() {
        std::lock_guard<std::mutex> lock(positions_mutex_);
        std::string exchange = config_.strategy.exchange;

        for (const auto& [symbol, pos] : positions_) {
            if (pos.net_quantity == 0) continue;

            Side close_side = (pos.net_quantity > 0) ? Side::SELL : Side::BUY;
            int qty = std::abs(pos.net_quantity);

            LOG_INFO("App", "Flattening " + symbol + ": " +
                     (close_side == Side::BUY ? "BUY" : "SELL") + " " +
                     std::to_string(qty));

            order_mgr_.send_market_order(symbol, exchange, close_side, qty);
        }

        // Cancel all pending orders
        order_mgr_.cancel_all();
    }

    void emergency_flatten(const std::string& symbol) {
        LOG_ERROR("App", "EMERGENCY FLATTEN: " + symbol);

        std::lock_guard<std::mutex> lock(positions_mutex_);
        auto it = positions_.find(symbol);
        if (it == positions_.end() || it->second.net_quantity == 0) return;

        Side close_side = (it->second.net_quantity > 0) ? Side::SELL : Side::BUY;
        int qty = std::abs(it->second.net_quantity);

        order_mgr_.cancel_all(symbol);
        order_mgr_.send_market_order(symbol, config_.strategy.exchange, close_side, qty);
    }
};

int main(int argc, char* argv[]) {
    std::string config_path = "config/strategy.ini";
    if (argc > 1) {
        config_path = argv[1];
    }

    std::cout << "======================================" << std::endl;
    std::cout << " ShieldOrder Rithmic Trading System" << std::endl;
    std::cout << " FDXM -> FDAX + FESX Strategy" << std::endl;
    std::cout << "======================================" << std::endl;

    // Install signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Load configuration
    Config config = load_config(config_path);

    // Create and run trading application
    TradingApp app(config);

    if (!app.initialize()) {
        std::cerr << "Initialization failed. Check logs." << std::endl;
        return 1;
    }

    app.run();

    return 0;
}
