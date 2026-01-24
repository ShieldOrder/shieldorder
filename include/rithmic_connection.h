#pragma once

#include "types.h"
#include "config.h"
#include <functional>
#include <atomic>
#include <thread>
#include <string>

namespace shieldorder {

// Callbacks for async Rithmic events
struct RithmicCallbacks {
    std::function<void(const Tick&)> on_tick;
    std::function<void(const DepthUpdate&)> on_depth;
    std::function<void(const Order&)> on_order_update;
    std::function<void(const Position&)> on_position_update;
    std::function<void(const std::string&)> on_error;
    std::function<void(bool)> on_connection_status;
};

class RithmicConnection {
public:
    explicit RithmicConnection(const RithmicConfig& config);
    ~RithmicConnection();

    // Connection lifecycle
    bool connect();
    void disconnect();
    bool is_connected() const;

    // Market data
    bool subscribe_market_data(const std::string& symbol, const std::string& exchange);
    bool subscribe_depth(const std::string& symbol, const std::string& exchange, int levels = 10);
    bool unsubscribe_market_data(const std::string& symbol, const std::string& exchange);

    // Order management
    std::string submit_order(const std::string& symbol, const std::string& exchange,
                             Side side, OrderType type, int quantity, double price = 0.0);
    bool cancel_order(const std::string& order_id);
    bool modify_order(const std::string& order_id, double new_price, int new_quantity);

    // Account info
    bool request_positions();
    bool request_account_info();

    // Set callbacks
    void set_callbacks(const RithmicCallbacks& callbacks);

private:
    RithmicConfig config_;
    RithmicCallbacks callbacks_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};
    std::thread heartbeat_thread_;

    // R|API+ handles will go here when SDK is integrated
    // RApi::REngine* engine_ = nullptr;
    // RApi::AdmCallbacks* adm_callbacks_ = nullptr;
    // etc.

    void heartbeat_loop();
    void handle_disconnect();
    bool attempt_reconnect(int max_retries = 5);
};

} // namespace shieldorder
