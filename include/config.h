#pragma once

#include <string>

namespace shieldorder {

struct RithmicConfig {
    std::string server_name;       // e.g., "Rithmic Paper Trading"
    std::string gateway;           // e.g., "login.rithmic.com"
    int port = 443;
    std::string user;
    std::string password;
    std::string system_name;       // Your system name from conformance
    std::string app_name;          // Your app name from conformance
    std::string app_version;
    std::string cert_file;
    std::string domain_server_address;
    std::string license_key;
};

struct StrategyConfig {
    // Instruments
    std::string mini_dax_symbol = "FDXM";
    std::string dax_symbol = "FDAX";
    std::string eurostoxx_symbol = "FESX";
    std::string exchange = "EUREX";

    // Signal generation
    int lookback_bars = 20;
    double momentum_threshold = 0.3;     // Minimum signal strength to enter
    double correlation_threshold = 0.85;  // DAX/Stoxx correlation threshold
    int tick_aggregation_ms = 500;        // Aggregate ticks into this interval

    // Scaling parameters
    int initial_mini_lots = 2;            // Start with N mini DAX contracts
    double scale_threshold = 0.6;         // Signal strength to scale to FDAX
    int max_fdax_lots = 2;                // Max full DAX contracts
    int max_fdxm_lots = 10;              // Max mini DAX contracts
    int max_fesx_lots = 5;               // Max Euro Stoxx contracts

    // Risk management
    double max_daily_loss_eur = 2000.0;
    double max_position_loss_eur = 500.0;
    double trailing_stop_points = 15.0;
    double profit_target_points = 30.0;
    int max_open_orders = 6;
    double max_spread_ticks = 3.0;       // Don't trade if spread > N ticks

    // Timing
    int start_hour_utc = 7;    // Eurex opens 07:00 UTC (08:00 CET)
    int end_hour_utc = 20;     // Stop new entries by 20:00 UTC
    int flatten_hour_utc = 21; // Flatten everything by 21:00 UTC
};

struct Config {
    RithmicConfig rithmic;
    StrategyConfig strategy;
};

Config load_config(const std::string& path);

} // namespace shieldorder
