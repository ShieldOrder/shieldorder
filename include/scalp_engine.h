#pragma once

#include "order_flow_types.h"
#include "tape_reader.h"
#include "footprint_engine.h"
#include "book_analyzer.h"
#include "market_structure.h"
#include "config.h"
#include <vector>
#include <functional>

namespace shieldorder {

// ============================================================================
// The Perfect Order Flow Scalper
//
// ENTRY CONDITIONS (confluence required):
// A trade triggers when multiple independent signals align:
//
// 1. ORDER FLOW (Footprint/Delta):
//    - Stacked imbalance in direction of trade
//    - Delta confirming direction (not diverging)
//    - Absorption at opposing level (trapped traders)
//
// 2. BOOK DYNAMICS:
//    - Imbalance favoring trade direction
//    - Stacking on entry side OR pulling on opposing side
//    - No large resting orders blocking the path
//
// 3. MARKET STRUCTURE:
//    - Trading in direction of structure (HH/HL for longs, LH/LL for shorts)
//    - Near FVG or liquidity level that provides context
//    - Not trading into obvious resistance/support
//
// 4. TAPE:
//    - Aggressive flow confirming direction
//    - Not in exhaustion/climactic state (avoid chasing)
//    - Large lots participating in direction
//
// 5. VOLUME CONTEXT:
//    - Trading relative to POC/VA (value vs premium/discount)
//    - Volume confirming move (not diverging)
//
// EXIT CONDITIONS:
//    - Target hit (structure-based or fixed points)
//    - Opposing signal develops (book flips, delta reverses)
//    - Exhaustion detected (climactic volume with no follow-through)
//    - Trailing stop moved to breakeven after +5 pts on DAX
//    - Time stop: exit if no progress within N seconds
// ============================================================================

class ScalpEngine {
public:
    struct Config {
        // Confluence weights (must sum to ~1.0)
        double weight_order_flow = 0.30;
        double weight_book = 0.25;
        double weight_structure = 0.20;
        double weight_tape = 0.15;
        double weight_volume = 0.10;

        // Entry thresholds
        double min_confluence = 0.55;       // Minimum combined score to enter
        double strong_confluence = 0.75;    // Strong signal - scale up
        int min_factors_aligned = 3;        // At least N of 5 factors must be positive

        // Scalp targets (DAX points)
        double default_target_pts = 8.0;    // Default take profit
        double tight_stop_pts = 4.0;        // Initial stop
        double breakeven_trigger_pts = 5.0; // Move stop to B/E after this
        double trail_distance_pts = 3.0;    // Trailing stop distance after B/E
        int time_stop_seconds = 60;         // Exit if no progress

        // Anti-chase filters
        double max_entry_spread_ticks = 2.0;
        bool require_absorption = false;    // Require absorption before entry
        bool avoid_climactic = true;        // Don't enter during climactic volume
        double max_velocity = 5.0;          // Don't chase fast moves (pts/sec)

        // FDXM -> FDAX scaling
        double scale_up_confidence = 0.80;  // Confidence to scale to FDAX
        int warmup_trades = 3;              // Min trades before scaling allowed
    };

    explicit ScalpEngine(const Config& config = Config{});

    // The main evaluation: given all analytics, produce a signal
    ScalpSignal evaluate(const std::string& symbol,
                         const TapeReader& tape,
                         const FootprintEngine& footprint,
                         const BookAnalyzer& book,
                         const MarketStructure& structure,
                         const VolumeProfile& profile,
                         double current_price);

    // Check if an existing position should be exited
    struct ExitSignal {
        bool should_exit;
        std::string reason;
        double suggested_exit_price;
    };
    ExitSignal check_exit(const std::string& symbol,
                          Side position_side,
                          double entry_price,
                          double current_price,
                          Timestamp entry_time,
                          const TapeReader& tape,
                          const FootprintEngine& footprint,
                          const BookAnalyzer& book);

    // Track trade outcomes for adaptation
    void record_trade_result(double pnl_points, double confidence_at_entry);

    // Performance stats
    struct Stats {
        int total_trades = 0;
        int winners = 0;
        int losers = 0;
        double total_pnl = 0.0;
        double avg_winner = 0.0;
        double avg_loser = 0.0;
        double win_rate() const { return total_trades > 0 ? (double)winners / total_trades : 0.0; }
    };
    const Stats& stats() const { return stats_; }

private:
    Config config_;
    Stats stats_;

    // Individual factor scoring
    double score_order_flow(const FootprintEngine& footprint, Side side) const;
    double score_book(const BookAnalyzer& book, Side side) const;
    double score_structure(const MarketStructure& structure, Side side, double price) const;
    double score_tape(const TapeReader& tape, Side side) const;
    double score_volume_context(const VolumeProfile& profile, Side side, double price) const;

    // Anti-chase and filter logic
    bool passes_entry_filters(const TapeReader& tape, const BookAnalyzer& book, double price) const;

    // Determine stop and target based on structure
    double compute_stop(Side side, double entry, const MarketStructure& structure) const;
    double compute_target(Side side, double entry, const MarketStructure& structure,
                          const VolumeProfile& profile) const;

    // Count how many factors are aligned
    int count_aligned_factors(double of_score, double book_score,
                              double struct_score, double tape_score,
                              double vol_score) const;
};

} // namespace shieldorder
