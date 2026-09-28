#pragma once

// The read side of decision_context_snapshots.
//
// The snapshot encoder has existed since the archive was introduced; nothing
// ever read it back. An archive that cannot be parsed is a write-only log, and
// "we can replay this later" was an assumption nobody had tested. This is the
// inverse of context_snapshot_json(), and the round-trip is pinned by a test.
//
// What comes back is the analytics the strategies read, plus the venue scalars
// stored in the table's own columns. What does NOT come back is anything the
// snapshot never carried -- projected zones and the ML forecast among them --
// and those stay ABSENT rather than being reconstructed from a later source.
// A replay that quietly filled them would be scoring strategies on information
// the live decision did not have.

#include "altstrategy/engine/decision_types.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace altstrategy::engine_io {

// Scalars stored as columns rather than inside the snapshot blob.
struct SnapshotScalars {
    int64_t canonical_pair_id = 0;
    int64_t bitget_instrument_id = 0;
    int64_t context_revision = 0;
    int64_t event_time_ms = 0;
    int64_t available_at_ms = 0;
    std::string data_quality;
    std::string universe_status;
    std::string feature_manifest_hash;
    uint32_t venue_mask = 0;
    uint32_t missing_venue_mask = 0;
    bool regime_divergence = false;
    double fees_bps = 4.0;
    double funding_rate = 0;
    double mark_price = 0;
    double spread_bps = 0;
    double top_depth_usdt = 0;
    double liquidity_rank = 0;
    double expected_slippage_bps = 0;
    bool spread_observed = false;
    bool depth_observed = false;
    std::string symbol;
};

// Rebuild the context a decision was taken on. Returns nullopt when the JSON is
// not an object or carries no Bitget analytics -- an unusable snapshot must be
// skipped, never replayed as an empty context that every strategy declines on
// (which would read as "no setups" instead of "no data").
[[nodiscard]] std::optional<engine::DecisionContext> context_from_snapshot(
    std::string_view snapshot_json, const SnapshotScalars& s);

} // namespace altstrategy::engine_io
