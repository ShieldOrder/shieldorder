#pragma once

#include "order_flow_types.h"
#include <deque>
#include <mutex>
#include <functional>
#include <unordered_map>

namespace shieldorder {

class BookAnalyzer {
public:
    struct Config {
        int snapshot_history = 100;          // How many snapshots to keep
        double pull_threshold_pct = 0.5;     // 50% size reduction = pull
        double stack_threshold_pct = 1.0;    // 100% size increase = stack
        int iceberg_refill_count = 3;        // Refills to confirm iceberg
        int sweep_levels = 3;               // Levels taken out = sweep
        int sweep_time_ms = 500;            // Sweep must happen within N ms
        double significant_size_mult = 3.0; // N * avg_level_size = significant
        int imbalance_levels = 5;           // Levels to consider for imbalance
    };

    explicit BookAnalyzer(const Config& config = Config{});

    // Process a new depth snapshot
    void on_depth(const DepthUpdate& update);

    // Process a trade (for absorption/iceberg detection)
    void on_trade(const ClassifiedTrade& trade);

    // Current book state
    const BookSnapshot& current_snapshot() const { return current_; }

    // Book imbalance (positive = bid heavy, negative = ask heavy)
    double imbalance(int levels = 5) const;

    // Detect current book events
    std::vector<BookEventRecord> get_recent_events(int max_age_ms = 5000) const;

    // Specific queries
    bool is_bid_stacking(double price_range = 5.0) const;
    bool is_ask_stacking(double price_range = 5.0) const;
    bool is_bid_pulling() const;
    bool is_ask_pulling() const;
    bool is_absorption_at_bid() const;
    bool is_absorption_at_ask() const;
    bool has_iceberg_bid() const;
    bool has_iceberg_ask() const;
    bool is_sweep_up() const;
    bool is_sweep_down() const;

    // Large resting orders (significant size on book)
    std::vector<BookLevel> get_significant_bids() const;
    std::vector<BookLevel> get_significant_asks() const;

    // Book pressure score: -1.0 (heavy selling pressure) to +1.0 (heavy buying pressure)
    double book_pressure_score() const;

    // Event callback
    void set_event_callback(std::function<void(const BookEventRecord&)> cb);

private:
    Config config_;
    mutable std::mutex mutex_;
    BookSnapshot current_;
    std::deque<BookSnapshot> snapshot_history_;
    std::deque<BookEventRecord> events_;
    std::function<void(const BookEventRecord&)> event_callback_;

    // Iceberg detection state
    struct IcebergState {
        double price;
        int refill_count;
        int last_size;
        Timestamp first_seen;
    };
    std::unordered_map<int, IcebergState> bid_icebergs_;  // price*100 -> state
    std::unordered_map<int, IcebergState> ask_icebergs_;

    // Absorption detection state
    struct AbsorptionState {
        double price;
        int volume_absorbed;
        int refills;
        Timestamp start;
    };
    AbsorptionState bid_absorption_state_;
    AbsorptionState ask_absorption_state_;

    void detect_stacking_pulling(const BookSnapshot& prev, const BookSnapshot& curr);
    void detect_sweeps(const BookSnapshot& prev, const BookSnapshot& curr);
    void detect_icebergs(const ClassifiedTrade& trade);
    void detect_absorption(const ClassifiedTrade& trade);
    void emit_event(BookEvent type, double price, int size, double significance);
    double avg_level_size() const;
    void trim_old_events();
};

} // namespace shieldorder
