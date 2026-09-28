#pragma once

// PostgreSQL access for the feature engine service: feature-set registration,
// candle reads for (re)plays, batched vector writes, checkpoints and runs.

#include "alttech/domain/venue.hpp"
#include "alttech/talib_features/feature_engine.hpp"
#include "alttech/persistence/pg.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace alttech::featsvc {

struct ShardCandle {
    talib_features::FBar bar;
    int64_t revision = 0;
    bool quality_ok = false;
};

struct VectorRow {
    int64_t instrument_id = 0;
    std::string timeframe;
    talib_features::FeatureVector vec;
    double bar_volume = 0;   // for the volume z-score in the flat projection
    int64_t bar_revision = 0;
    std::string quality = "VALID";
    std::string engine_version = talib_features::kEngineVersion;
    int64_t computed_at_ms = 0;
};

// Registers (or verifies) the talib-core feature set; returns its id.
Result<int64_t> ensure_core_feature_set(persistence::PgPool& pool);

// Candles for one shard ordered by open_time, [from_ms, to_ms), limited chunk.
Result<std::vector<ShardCandle>> load_candles(persistence::PgPool& pool, Venue venue, int64_t instrument_id,
                                              const std::string& tf_label, int64_t from_ms,
                                              int64_t to_ms, int limit);

// Current source-of-truth candle for one exact bar.  nullopt means the storage
// writer has not committed it yet.
// Load exactly the named bars. Used where a caller must verify a known set of
// bars rather than walk a window — see candles_at_query() for why a ranged
// query with a LIMIT is the wrong tool for that job.
Result<std::vector<ShardCandle>> load_candles_at(persistence::PgPool& pool, Venue venue,
                                                int64_t instrument_id,
                                                const std::string& tf_label,
                                                const std::vector<int64_t>& open_times_ms);

Result<std::optional<ShardCandle>> load_persisted_candle(persistence::PgPool& pool, Venue venue,
                                                        int64_t instrument_id,
                                                        const std::string& tf_label,
                                                        int64_t open_time_ms,
                                                        int64_t span_ms);

// Latest stored vector bar_open_time for a shard (or nullopt).
Result<std::optional<int64_t>> last_vector_time(persistence::PgPool& pool, Venue venue, int64_t instrument_id,
                                                const std::string& tf_label,
                                                int64_t feature_set_id);

// Earliest closed candle at or after from_ms that has no vector for its current
// revision. The lower bound must match the configured replay horizon so an
// intentionally excluded historical row cannot become a permanent hole.
Result<std::optional<int64_t>> first_missing_vector_time(
    persistence::PgPool& pool, Venue venue, int64_t instrument_id,
    const std::string& tf_label,
    int64_t feature_set_id, int64_t from_ms);

// Whether a vector exists for the current persisted revision of this bar.
// Earliest stored vector that is still WARMING while the source now has enough
// history behind it to converge.
//
// A vector records the state of the world when it was computed, and that state
// can improve underneath it. AltSpotter deepened its 1m history from 60 to 280
// days; the 1d vectors computed before that carried 733 of 995 features and
// were published WARMING, and nothing ever revisited them — `first_missing`
// only looks for ABSENT vectors, and these were present. AltML copies only
// `status = 'VALID'`, so those bars stayed excluded from every downstream
// consumer even though the data to fix them had arrived.
//
// `min_context_bars` is how many closed bars must precede a vector before its
// WARMING status is considered stale rather than genuine (a vector right after
// a gap is legitimately WARMING and must not be rewritten forever).
Result<std::optional<int64_t>> first_stale_warming_vector_time(
    persistence::PgPool& pool, Venue venue, int64_t instrument_id, const std::string& tf_label,
    int64_t feature_set_id, int64_t from_ms, int64_t min_context_bars);

Result<bool> current_vector_exists(persistence::PgPool& pool, Venue venue, int64_t instrument_id,
                                   const std::string& tf_label, int64_t open_time_ms,
                                   int64_t feature_set_id);

// Load one exact durable vector for at-least-once republish after an ACK or
// transport failure. The requested revision must still be PostgreSQL-current.
Result<std::optional<VectorRow>> load_current_vector(
    persistence::PgPool& pool, Venue venue, int64_t instrument_id,
    const std::string& tf_label,
    int64_t open_time_ms, int64_t feature_set_id, int64_t bar_revision);

// True when every persisted candle in [from_ms, to_ms) has a vector for its
// current revision.  Used as the durable completion condition before a batch
// message is ACKed.
Result<bool> current_vectors_complete(persistence::PgPool& pool, Venue venue, int64_t instrument_id,
                                      const std::string& tf_label, int64_t from_ms,
                                      int64_t to_ms, int64_t feature_set_id);

// The EARLIEST persisted candle in [from_ms, to_ms) that has no vector for its
// current revision, or nullopt when every one of them does.
//
// This is the difference between rebuilding what is actually missing and
// rebuilding the whole window a re-publication happened to name. AltSpotter's
// aggregator re-announces a rolling reconciliation horizon (35 minutes by
// default) on every cycle so a late 1m revision cannot be missed. For any
// timeframe whose horizon spans more than four windows that arrives as a BATCH,
// and treating a batch that merely overlaps processed history as a reason to
// rebuild from its first bar turned one genuinely new bar into a seven-bar
// rebuild, per symbol, every five minutes, forever.
Result<std::optional<int64_t>> first_incomplete_bar(persistence::PgPool& pool, Venue venue,
                                                    int64_t instrument_id,
                                                    const std::string& tf_label, int64_t from_ms,
                                                    int64_t to_ms, int64_t feature_set_id);

// Batched UPSERT of vectors + the small core projection.
Result<int64_t> write_vectors(persistence::PgPool& pool, Venue venue, int64_t feature_set_id,
                              const std::vector<VectorRow>& rows,
                              const std::map<std::string, size_t>& name_index);

// Progress checkpoint.  The state marker explicitly declares that the engine
// is rebuilt from a bounded contiguous candle replay; it is never presented as
// serialized indicator state.
Result<void> save_checkpoint(persistence::PgPool& pool, Venue venue, int64_t instrument_id,
                             const std::string& tf_label, int64_t last_bar_ms);

Result<int64_t> open_run(persistence::PgPool& pool, Venue venue, const char* mode, int64_t instrument_id,
                         const std::string& tf_label);
void close_run(persistence::PgPool& pool, int64_t run_id, int64_t vectors, bool ok,
               const std::string& error);

} // namespace alttech::featsvc
