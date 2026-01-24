#include "order_manager.h"
#include "logger.h"
#include <algorithm>

namespace shieldorder {

OrderManager::OrderManager(RithmicConnection& connection)
    : connection_(connection) {}

std::string OrderManager::send_market_order(const std::string& symbol, const std::string& exchange,
                                            Side side, int quantity) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string id = next_order_id();
    Order order;
    order.order_id = id;
    order.symbol = symbol;
    order.side = side;
    order.type = OrderType::MARKET;
    order.price = 0.0;
    order.quantity = quantity;
    order.filled_quantity = 0;
    order.state = OrderState::PENDING;
    order.submit_time = std::chrono::steady_clock::now();

    orders_[id] = order;

    std::string rithmic_id = connection_.submit_order(symbol, exchange, side, OrderType::MARKET, quantity);
    orders_[id].rithmic_order_id = rithmic_id;
    orders_[id].state = OrderState::SENT;

    LOG_INFO("OrderMgr", "Market order sent: " + id + " " +
             (side == Side::BUY ? "BUY" : "SELL") + " " +
             std::to_string(quantity) + " " + symbol);

    return id;
}

std::string OrderManager::send_limit_order(const std::string& symbol, const std::string& exchange,
                                            Side side, int quantity, double price) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string id = next_order_id();
    Order order;
    order.order_id = id;
    order.symbol = symbol;
    order.side = side;
    order.type = OrderType::LIMIT;
    order.price = price;
    order.quantity = quantity;
    order.filled_quantity = 0;
    order.state = OrderState::PENDING;
    order.submit_time = std::chrono::steady_clock::now();

    orders_[id] = order;

    std::string rithmic_id = connection_.submit_order(symbol, exchange, side, OrderType::LIMIT, quantity, price);
    orders_[id].rithmic_order_id = rithmic_id;
    orders_[id].state = OrderState::SENT;

    LOG_INFO("OrderMgr", "Limit order sent: " + id + " " +
             (side == Side::BUY ? "BUY" : "SELL") + " " +
             std::to_string(quantity) + " " + symbol + " @ " + std::to_string(price));

    return id;
}

std::string OrderManager::send_stop_order(const std::string& symbol, const std::string& exchange,
                                           Side side, int quantity, double stop_price) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string id = next_order_id();
    Order order;
    order.order_id = id;
    order.symbol = symbol;
    order.side = side;
    order.type = OrderType::STOP;
    order.price = stop_price;
    order.quantity = quantity;
    order.filled_quantity = 0;
    order.state = OrderState::PENDING;
    order.submit_time = std::chrono::steady_clock::now();

    orders_[id] = order;

    std::string rithmic_id = connection_.submit_order(symbol, exchange, side, OrderType::STOP, quantity, stop_price);
    orders_[id].rithmic_order_id = rithmic_id;
    orders_[id].state = OrderState::SENT;

    LOG_INFO("OrderMgr", "Stop order sent: " + id + " " +
             (side == Side::BUY ? "BUY" : "SELL") + " " +
             std::to_string(quantity) + " " + symbol + " stop@ " + std::to_string(stop_price));

    return id;
}

bool OrderManager::cancel(const std::string& order_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = orders_.find(order_id);
    if (it == orders_.end()) return false;
    if (it->second.state == OrderState::FILLED || it->second.state == OrderState::CANCELLED) return false;

    bool result = connection_.cancel_order(it->second.rithmic_order_id);
    if (result) {
        LOG_INFO("OrderMgr", "Cancel requested: " + order_id);
    }
    return result;
}

bool OrderManager::cancel_all(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool all_ok = true;

    for (auto& [id, order] : orders_) {
        if (order.state == OrderState::FILLED || order.state == OrderState::CANCELLED) continue;
        if (!symbol.empty() && order.symbol != symbol) continue;

        if (!connection_.cancel_order(order.rithmic_order_id)) {
            all_ok = false;
        }
    }

    LOG_INFO("OrderMgr", "Cancel all requested" + (symbol.empty() ? "" : " for " + symbol));
    return all_ok;
}

bool OrderManager::modify_price(const std::string& order_id, double new_price) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = orders_.find(order_id);
    if (it == orders_.end()) return false;
    if (it->second.state == OrderState::FILLED || it->second.state == OrderState::CANCELLED) return false;

    return connection_.modify_order(it->second.rithmic_order_id, new_price, it->second.quantity);
}

OrderManager::BracketResult OrderManager::send_bracket(
    const std::string& symbol, const std::string& exchange,
    Side side, int quantity, double entry_price,
    double stop_price, double target_price) {

    BracketResult result;

    // Entry order (limit)
    result.entry_id = send_limit_order(symbol, exchange, side, quantity, entry_price);

    // Protective stop (opposite side)
    Side stop_side = (side == Side::BUY) ? Side::SELL : Side::BUY;
    result.stop_id = send_stop_order(symbol, exchange, stop_side, quantity, stop_price);

    // Profit target (opposite side, limit)
    result.target_id = send_limit_order(symbol, exchange, stop_side, quantity, target_price);

    LOG_INFO("OrderMgr", "Bracket sent for " + symbol + ": entry=" + result.entry_id +
             " stop=" + result.stop_id + " target=" + result.target_id);

    return result;
}

const Order& OrderManager::get_order(const std::string& order_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    static Order empty;
    auto it = orders_.find(order_id);
    return (it != orders_.end()) ? it->second : empty;
}

std::vector<Order> OrderManager::get_open_orders(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Order> result;

    for (const auto& [id, order] : orders_) {
        if (order.state == OrderState::FILLED || order.state == OrderState::CANCELLED ||
            order.state == OrderState::REJECTED) continue;
        if (!symbol.empty() && order.symbol != symbol) continue;
        result.push_back(order);
    }

    return result;
}

int OrderManager::open_order_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    int count = 0;
    for (const auto& [id, order] : orders_) {
        if (order.state != OrderState::FILLED && order.state != OrderState::CANCELLED &&
            order.state != OrderState::REJECTED) {
            count++;
        }
    }
    return count;
}

bool OrderManager::has_pending_orders(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [id, order] : orders_) {
        if (order.symbol != symbol) continue;
        if (order.state == OrderState::PENDING || order.state == OrderState::SENT ||
            order.state == OrderState::ACKNOWLEDGED) {
            return true;
        }
    }
    return false;
}

void OrderManager::on_order_update(const Order& update) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Find by rithmic order ID
    for (auto& [id, order] : orders_) {
        if (order.rithmic_order_id == update.rithmic_order_id) {
            order.state = update.state;
            order.filled_quantity = update.filled_quantity;

            std::string state_str;
            switch (update.state) {
                case OrderState::ACKNOWLEDGED: state_str = "ACK"; break;
                case OrderState::PARTIAL_FILL: state_str = "PARTIAL"; break;
                case OrderState::FILLED: state_str = "FILLED"; break;
                case OrderState::CANCELLED: state_str = "CANCELLED"; break;
                case OrderState::REJECTED: state_str = "REJECTED"; break;
                default: state_str = "UNKNOWN"; break;
            }

            LOG_INFO("OrderMgr", "Order update: " + id + " -> " + state_str +
                     " (filled=" + std::to_string(order.filled_quantity) + "/" +
                     std::to_string(order.quantity) + ")");
            return;
        }
    }

    LOG_WARN("OrderMgr", "Received update for unknown order: " + update.rithmic_order_id);
}

std::string OrderManager::next_order_id() {
    return "SO-" + std::to_string(++order_sequence_);
}

} // namespace shieldorder
