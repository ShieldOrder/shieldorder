#include "config.h"
#include "logger.h"
#include <fstream>
#include <sstream>
#include <algorithm>

namespace shieldorder {

namespace {

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

} // anonymous namespace

Config load_config(const std::string& path) {
    Config cfg;
    std::ifstream file(path);
    if (!file.is_open()) {
        LOG_ERROR("Config", "Failed to open config file: " + path);
        return cfg;
    }

    std::string line;
    std::string section;

    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        // Section header
        if (line[0] == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }

        auto eq_pos = line.find('=');
        if (eq_pos == std::string::npos) continue;

        std::string key = trim(line.substr(0, eq_pos));
        std::string val = trim(line.substr(eq_pos + 1));

        if (section == "rithmic") {
            if (key == "server_name") cfg.rithmic.server_name = val;
            else if (key == "gateway") cfg.rithmic.gateway = val;
            else if (key == "port") cfg.rithmic.port = std::stoi(val);
            else if (key == "user") cfg.rithmic.user = val;
            else if (key == "password") cfg.rithmic.password = val;
            else if (key == "system_name") cfg.rithmic.system_name = val;
            else if (key == "app_name") cfg.rithmic.app_name = val;
            else if (key == "app_version") cfg.rithmic.app_version = val;
            else if (key == "cert_file") cfg.rithmic.cert_file = val;
            else if (key == "domain_server_address") cfg.rithmic.domain_server_address = val;
            else if (key == "license_key") cfg.rithmic.license_key = val;
        }
        else if (section == "strategy") {
            if (key == "mini_dax_symbol") cfg.strategy.mini_dax_symbol = val;
            else if (key == "dax_symbol") cfg.strategy.dax_symbol = val;
            else if (key == "eurostoxx_symbol") cfg.strategy.eurostoxx_symbol = val;
            else if (key == "exchange") cfg.strategy.exchange = val;
            else if (key == "lookback_bars") cfg.strategy.lookback_bars = std::stoi(val);
            else if (key == "momentum_threshold") cfg.strategy.momentum_threshold = std::stod(val);
            else if (key == "correlation_threshold") cfg.strategy.correlation_threshold = std::stod(val);
            else if (key == "tick_aggregation_ms") cfg.strategy.tick_aggregation_ms = std::stoi(val);
            else if (key == "initial_mini_lots") cfg.strategy.initial_mini_lots = std::stoi(val);
            else if (key == "scale_threshold") cfg.strategy.scale_threshold = std::stod(val);
            else if (key == "max_fdax_lots") cfg.strategy.max_fdax_lots = std::stoi(val);
            else if (key == "max_fdxm_lots") cfg.strategy.max_fdxm_lots = std::stoi(val);
            else if (key == "max_fesx_lots") cfg.strategy.max_fesx_lots = std::stoi(val);
            else if (key == "max_daily_loss_eur") cfg.strategy.max_daily_loss_eur = std::stod(val);
            else if (key == "max_position_loss_eur") cfg.strategy.max_position_loss_eur = std::stod(val);
            else if (key == "trailing_stop_points") cfg.strategy.trailing_stop_points = std::stod(val);
            else if (key == "profit_target_points") cfg.strategy.profit_target_points = std::stod(val);
            else if (key == "max_open_orders") cfg.strategy.max_open_orders = std::stoi(val);
            else if (key == "max_spread_ticks") cfg.strategy.max_spread_ticks = std::stod(val);
            else if (key == "start_hour_utc") cfg.strategy.start_hour_utc = std::stoi(val);
            else if (key == "end_hour_utc") cfg.strategy.end_hour_utc = std::stoi(val);
            else if (key == "flatten_hour_utc") cfg.strategy.flatten_hour_utc = std::stoi(val);
        }
    }

    LOG_INFO("Config", "Configuration loaded from: " + path);
    return cfg;
}

} // namespace shieldorder
