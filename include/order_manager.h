#pragma once

#include "types.h"
#include "rithmic_connection.h"
#include <unordered_map>
#include <mutex>
#include <vector>

namespace shieldorder {

class OrderManager {
public:
    explicit OrderManager(RithmicConnection& connection);

    // Order submission
    std::string send_market_order(const std::string& symbol, const std::string& exchange,
                                  Side side, int quantity);
    std::string send_limit_order(const std::string& symbol, const std::string& exchange,
                                  Side side, int quantity, double price);
    std::string send_stop_order(const std::string& symbol, const std::string& exchange,
                                Side side, int quantity, double stop_price);

    // Order management
    bool cancel(const std::string& order_id);
    bool cancel_all(const std::string& symbol = "");
    bool modify_price(const std::string& order_id, double new_price);

    // Bracket orders (entry + stop + target)
    struct BracketResult {
        std::string entry_id;
        std::string stop_id;
        std::string target_id;
    };
    BracketResult send_bracket(const std::string& symbol, const std::string& exchange,
                               Side side, int quantity, double entry_price,
                               double stop_price, double target_price);

    // State queries
    const Order& get_order(const std::string& order_id) const;
    std::vector<Order> get_open_orders(const std::string& symbol = "") const;
    int open_order_count() const;
    bool has_pending_orders(const std::string& symbol) const;

    // Called by Rithmic callbacks
    void on_order_update(const Order& update);

private:
    RithmicConnection& connection_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Order> orders_;
    int order_sequence_ = 0;

    std::string next_order_id();
};

} // namespace shieldorder
