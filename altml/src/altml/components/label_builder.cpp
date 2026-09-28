// label-builder component: builds future-outcome labels for closed 1m bars.
//
// Labels (label set "core-labels", horizons in minutes):
//   future_return_{1,3,5,10,15,30}m           log(close[t+h] / close[t])
//   future_return_after_fees_{1,5,15,30}m     minus round-trip taker fees
//   max_favorable/adverse_excursion_{5,15,30}m  (long perspective, vs close[t])
//   future_volatility_{5,15,30}m              stddev of 1m log returns
//   direction_{5,15,30}m                      sign(close[t+h] - close[t])
//   target_before_stop, time_to_target, time_to_stop
//       barrier experiment: target = close*(1 + k*ATRn), stop = close*(1 - k*ATRn)
//       with k=1.0, ATR proxy = rolling mean TR(14) computed CAUSALLY (bars <= t);
//       horizon 30m; if both barriers hit inside one bar -> conservative rule:
//       stop first (AMBIGUOUS bars never resolve in our favor).
//
// Leakage discipline: label values use ONLY bars strictly after t (barrier ATR
// uses bars <= t and is part of the label definition, not a feature).
// A bar is labeled once bar t+30m is closed. Fees are versioned via
// altml.fee_models. Everything is idempotent (UPSERT by natural key).
//
// Label set "econ-labels" (the ML-first target; see econ_logic.hpp), per
// horizon h in {15m, 1h, 4h}:
//   log_return_{h}                 log(close[t+h] / close[t])
//   mfe_long_{h}, mae_long_{h}     log of the extreme high / low over t+1..t+h
//   minutes_to_mfe_{h}, minutes_to_mae_{h}
//   econ_class_{h}                 0 SHORT / 1 NEUTRAL / 2 LONG against the
//                                  all-in round trip of the fee model
// The short side is the mirror of the long one and is not stored twice. The
// same builder loop produces both sets, so both inherit one definition of a
// complete horizon and one revision audit.

#include "altml/components/label_builder.hpp"

#include "altml/components/econ_logic.hpp"
#include "altml/components/label_quality.hpp"
#include "altml/core/hash.hpp"
#include "altml/core/logger.hpp"
#include "altml/core/pg.hpp"
#include "altml/core/readiness.hpp"
#include "altml/core/shutdown.hpp"
#include "altml/core/time.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace altml {

namespace {

constexpr const char* kInitialAtrRepairRun =
    "ml-label-builder-service:initial-atr-repair:lb-1.0.0";
constexpr double kBarrierAtrMult = 1.0;

// One frozen label set: what it is called, how far into the future it reads,
// how much causal history it needs before the anchor, and its catalogue.
struct LabelSetSpec {
    std::string name;
    int version = 1;
    std::string builder_version;
    int max_horizon = 0;      // minutes of future a label reads
    int warmup_minutes = 0;   // causal state before the anchor (ATR); 0 = none
    // A new set on a database whose candle history is months long does not
    // start at the first candle: labels older than the feature archive pair
    // with nothing and would be retained only to be dropped. 0 = from the
    // first candle (the historical behaviour of core-labels).
    int backfill_days = 0;
    std::string kind;         // altml.label_definitions.kind
    bool core = false;
    std::vector<std::pair<std::string, int>> defs;
};

const std::vector<std::pair<std::string, int>>& label_defs() {
    static const std::vector<std::pair<std::string, int>> defs = {
        {"future_return_1m", 1},   {"future_return_3m", 3},   {"future_return_5m", 5},
        {"future_return_10m", 10}, {"future_return_15m", 15}, {"future_return_30m", 30},
        {"future_return_after_fees_1m", 1},  {"future_return_after_fees_5m", 5},
        {"future_return_after_fees_15m", 15}, {"future_return_after_fees_30m", 30},
        {"max_favorable_excursion_5m", 5},   {"max_adverse_excursion_5m", 5},
        {"max_favorable_excursion_15m", 15}, {"max_adverse_excursion_15m", 15},
        {"max_favorable_excursion_30m", 30}, {"max_adverse_excursion_30m", 30},
        {"future_volatility_5m", 5},  {"future_volatility_15m", 15},
        {"future_volatility_30m", 30},
        {"direction_5m", 5}, {"direction_15m", 15}, {"direction_30m", 30},
        {"target_before_stop", 30}, {"time_to_target", 30}, {"time_to_stop", 30},
    };
    return defs;
}

const LabelSetSpec& core_spec() {
    static const LabelSetSpec spec = [] {
        LabelSetSpec s;
        s.name = "core-labels";
        s.version = 1;
        s.builder_version = "lb-1.0.0";
        s.max_horizon = 30;
        s.warmup_minutes = 14;
        s.kind = "core";
        s.core = true;
        s.defs = label_defs();
        return s;
    }();
    return spec;
}

const LabelSetSpec& econ_spec() {
    static const LabelSetSpec spec = [] {
        LabelSetSpec s;
        s.name = "econ-labels";
        s.version = 1;
        s.builder_version = "lb-econ-1.0.0";
        s.max_horizon = econ::kMaxHorizonMinutes;
        s.warmup_minutes = 0;
        s.backfill_days = 41;   // the feature archive's 40-day target, plus one
        s.kind = "econ";
        s.core = false;
        for (const auto& h : econ::kHorizons) {
            const std::string n(h.name);
            s.defs.emplace_back("log_return_" + n, h.minutes);
            s.defs.emplace_back("mfe_long_" + n, h.minutes);
            s.defs.emplace_back("mae_long_" + n, h.minutes);
            s.defs.emplace_back("minutes_to_mfe_" + n, h.minutes);
            s.defs.emplace_back("minutes_to_mae_" + n, h.minutes);
            s.defs.emplace_back("econ_class_" + n, h.minutes);
        }
        return s;
    }();
    return spec;
}

using LBar = labels::SourceBar;

// Retention target is 150 days (see altml.retention_policies); keep a wide
// forward margin so a stalled service never wakes up to a missing partition.
constexpr int64_t kPartitionBackfillDays = 160;
constexpr int64_t kPartitionLookaheadDays = 60;
// The feature archive is partitioned by DAY, so its window is counted in
// partitions, not in calendar reach: 160 days back would mean 160 tables. It
// needs only enough history to cover its own retention target plus margin.
constexpr int64_t kArchivePartitionBackfillDays = 35;
constexpr int64_t kArchivePartitionLookaheadDays = 7;

Result<void> ensure_data_partitions(persistence::PgPool& pool, int64_t now_ms) {
    const int64_t day_ms = 24LL * 3600 * 1000;
    const int64_t from_ms = now_ms - kPartitionBackfillDays * day_ms;
    const int64_t to_ms = now_ms + kPartitionLookaheadDays * day_ms;
    return pool.with_txn([&](pqxx::work& txn) {
        txn.exec_params("SELECT altml.ensure_month_partitions('altml.labels', "
                        "to_timestamp($1::bigint / 1000.0), to_timestamp($2::bigint / 1000.0))",
                        from_ms, to_ms);
        // Live model output: only forward coverage matters. The table is part
        // of db/schema.sql; a database without it has not had this release's
        // schema applied, and saying so at start-up beats losing predictions.
        txn.exec_params("SELECT altml.ensure_month_partitions('altml.model_predictions', "
                        "to_timestamp($1::bigint / 1000.0), to_timestamp($2::bigint / 1000.0))",
                        now_ms - 31LL * day_ms, to_ms);
        // Daily: at ~1.1 GB/day across four venues and seven grids, a monthly
        // partition would be larger than the entire storage budget, leaving
        // retention with no usable lever. See altml.ensure_day_partitions.
        txn.exec_params("SELECT altml.ensure_day_partitions('altml.feature_vectors', "
                        "to_timestamp($1::bigint / 1000.0), to_timestamp($2::bigint / 1000.0))",
                        now_ms - kArchivePartitionBackfillDays * day_ms,
                        now_ms + kArchivePartitionLookaheadDays * day_ms);
    });
}

// Without this, postgres_fdw's foreign tables carry no local statistics (a
// plain ANALYZE never touches them automatically), so the planner assumes a
// ~1-row result for every query against them and can pick a catastrophic
// nested-loop join once the remote table is actually large — every join with
// altml.fdw_candles_1m timed out or hung for hours until this ran once. Since
// AltSpotter's candle history keeps growing, this must run periodically, not
// just at bootstrap.
Result<void> refresh_foreign_table_statistics(persistence::PgPool& pool) {
    return pool.with_txn([&](pqxx::work& txn) {
        txn.exec("ANALYZE altml.fdw_candles_1m");
        txn.exec("ANALYZE altml.fdw_exchange_instruments");
        txn.exec("ANALYZE altml.fdw_exchanges");
        txn.exec("ANALYZE altml.fdw_feature_set_versions");
        txn.exec("ANALYZE altml.fdw_feature_set_members");
    });
}

struct LabelRow {
    int64_t instrument_id = 0;
    int64_t bar_open_ms = 0;
    int64_t bar_revision = 0;
    std::vector<float> values;
    std::vector<uint8_t> validity;
};

struct LabelProgress {
    int64_t rows_written = 0;
    int64_t range_from_ms = 0;
    int64_t range_to_ms = 0;
    bool caught_up = true;
};

constexpr std::string_view kRevisionAuditRun = "ml-label-revision-audit";

class LabelBuilder {
public:
    LabelBuilder(persistence::PgPool& pool, const LabelSetSpec& spec, double taker_fee,
                 double slippage_estimate_bps, int fee_model_version)
        : pool_(pool), spec_(spec), round_trip_fee_(2.0 * taker_fee),
          econ_cost_(econ::cost_from_fee_model(taker_fee, slippage_estimate_bps).fraction()),
          fee_model_version_(fee_model_version) {
        std::string joined;
        for (const auto& [n, h] : spec_.defs) {
            joined += n;
            joined += ':';
            joined += std::to_string(h);
            joined += '\n';
        }
        schema_hash_ = sha256_hex(joined);
    }

    Result<int64_t> ensure_set() {
        int64_t id = 0;
        std::string existing;
        std::string existing_builder;
        int existing_fee_model = 0;
        bool existing_frozen = false;
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            const auto rows = txn.exec_params(
                "SELECT id, schema_hash, fee_model_version, builder_version, frozen "
                "FROM altml.label_set_versions "
                "WHERE name = $1 AND version = $2",
                spec_.name, spec_.version);
            if (!rows.empty()) {
                id = rows[0][0].as<int64_t>();
                existing = rows[0][1].as<std::string>();
                existing_fee_model = rows[0][2].as<int>();
                existing_builder = rows[0][3].as<std::string>();
                existing_frozen = rows[0][4].as<bool>();
                return;
            }
            const auto ins = txn.exec_params(
                "INSERT INTO altml.label_set_versions (name, version, fee_model_version, "
                "builder_version, schema_hash, frozen) VALUES ($1, $2, $3, $4, $5, true) "
                "RETURNING id",
                spec_.name, spec_.version, fee_model_version_,
                spec_.builder_version, schema_hash_);
            id = ins[0][0].as<int64_t>();
            const auto& defs = spec_.defs;
            for (size_t i = 0; i < defs.size(); ++i) {
                const auto d = txn.exec_params(
                    "INSERT INTO altml.label_definitions (name, horizon_minutes, kind) "
                    "VALUES ($1, $2, $3) ON CONFLICT (name) DO UPDATE SET "
                    "horizon_minutes = EXCLUDED.horizon_minutes RETURNING id",
                    defs[i].first, defs[i].second, spec_.kind);
                txn.exec_params(
                    "INSERT INTO altml.label_set_members (label_set_version_id, position, "
                    "definition_id, output_name) VALUES ($1, $2, $3, $4)",
                    id, static_cast<int>(i), d[0][0].as<int64_t>(), defs[i].first);
            }
        });
        if (!res) return std::unexpected(res.error());
        if (!existing.empty() && existing != schema_hash_) {
            return fail(ErrorCode::config_error, spec_.name + " frozen but catalog changed");
        }
        if (existing_fee_model != 0 && existing_fee_model != fee_model_version_) {
            return fail(ErrorCode::config_error,
                        spec_.name + " frozen with a different fee model; bump label-set version");
        }
        if (!existing_builder.empty() && existing_builder != spec_.builder_version) {
            return fail(ErrorCode::config_error,
                        spec_.name + " frozen with different label semantics; bump version");
        }
        if (!existing.empty() && !existing_frozen) {
            return fail(ErrorCode::config_error, spec_.name + " must remain frozen");
        }
        set_id_ = id;
        return id;
    }

    // Labels one instrument incrementally.  The periodic audit also rewinds to
    // the earliest row affected by a candle revision or a repaired gap.
    Result<LabelProgress> process_instrument(int64_t instrument_id, int64_t row_limit,
                                             int64_t revision_audit_since_ms,
                                             bool force_from_first_bar = false) {
        LabelProgress progress;
        if (row_limit <= 0) return progress;

        // Normal resume point is after the newest label for the CURRENT candle
        // revision.  On a new database, begin at the earliest candle: using
        // "now" here would seed historical ATR with future bars.
        int64_t from_ms = 0;
        {
            // The resume point: after the newest label whose candle revision is
            // still current. It was one join of EVERY label of the instrument
            // with the FDW candle table, which the planner ran as a nested loop
            // of remote lookups -- 58 809 of them, 37 s per instrument, for a
            // label set that is still filling (and growing with it). Now: the
            // newest label from the local index, then the revision check over
            // the last day only, as ONE remote query with a shipped time bound.
            // A revision older than that day is the revision audit's job (it
            // scans every candle changed since the last audit).
            auto resume = pool_.with_txn([&](pqxx::work& txn) {
                txn.exec("SET LOCAL statement_timeout = '30s'");
                txn.exec("SET LOCAL enable_nestloop = off");
                const auto hi = txn.exec_params(
                    "SELECT (extract(epoch FROM max(bar_open_time)) * 1000)::bigint "
                    "FROM altml.labels WHERE exchange_id = 1 AND instrument_id = $1 "
                    "AND timeframe = '1m' AND label_set_version_id = $2",
                    instrument_id, set_id_);
                if (hi.empty() || hi[0][0].is_null()) return;
                const int64_t lo_ms = hi[0][0].as<int64_t>() - 86'400'000LL;
                const auto r = txn.exec_params(
                    "SELECT (extract(epoch FROM max(l.bar_open_time)) * 1000)::bigint "
                    "FROM altml.labels l JOIN altml.fdw_candles_1m c "
                    "ON c.exchange_id = l.exchange_id AND c.instrument_id = l.instrument_id "
                    "AND c.open_time = l.bar_open_time AND c.revision = l.bar_revision "
                    "WHERE l.exchange_id = 1 AND l.instrument_id = $1 AND l.timeframe = '1m' "
                    "AND l.label_set_version_id = $2 "
                    "AND l.bar_open_time >= to_timestamp($3::bigint / 1000.0) "
                    "AND c.exchange_id = 1 AND c.instrument_id = $1 "
                    "AND c.open_time >= to_timestamp($3::bigint / 1000.0)",
                    instrument_id, set_id_, lo_ms);
                // No current-revision label in the last day: recompute the day.
                from_ms = (!r.empty() && !r[0][0].is_null()) ? r[0][0].as<int64_t>() + 60'000
                                                              : lo_ms;
            });
            if (!resume) return std::unexpected(resume.error());
        }
        {
            auto res = pool_.with_txn([&](pqxx::work& txn) {
                // A pathological audit must never make SIGTERM wait past the
                // systemd stop deadline. The indexed audit below normally
                // finishes in milliseconds; this is a final safety bound.
                txn.exec("SET LOCAL statement_timeout = '30s'");
                txn.exec("SET LOCAL lock_timeout = '5s'");

                if (from_ms == 0) {
                    // `>= floor` keeps a months-long candle history from
                    // being labeled for a new set; floor 0 is "no floor".
                    const int64_t floor_ms =
                        spec_.backfill_days > 0
                            ? timeutil::now_ms() - static_cast<int64_t>(spec_.backfill_days) *
                                                       86'400'000LL
                            : 0;
                    const auto first = txn.exec_params(
                        "SELECT (extract(epoch FROM min(open_time)) * 1000)::bigint "
                        "FROM altml.fdw_candles_1m WHERE exchange_id = 1 AND instrument_id = $1 "
                        "AND is_closed AND open_time >= to_timestamp($2::bigint / 1000.0)",
                        instrument_id, floor_ms);
                    if (!first.empty() && !first[0][0].is_null()) {
                        from_ms = first[0][0].as<int64_t>();
                    }
                }

                if (revision_audit_since_ms > 0) {
                    // A changed/inserted candle affects labels in
                    // [open - max_horizon, open + warmup] (future-return
                    // horizon plus causal ATR warmup; 30m and 14m for
                    // core-labels, 240m and none for econ-labels). Restrict the scan to changes since the last
                    // durably completed audit; the old per-label correlated
                    // scan was O(labels*45) and could occupy PostgreSQL for
                    // many minutes. Recomputing from the earliest affected
                    // label forward is conservative and restores ATR state.
                    const auto stale = txn.exec_params(
                        "WITH label_bounds AS MATERIALIZED ("
                        " SELECT min(bar_open_time) AS lo, max(bar_open_time) AS hi "
                        " FROM altml.labels WHERE exchange_id = 1 AND instrument_id = $1 "
                        " AND timeframe = '1m' AND label_set_version_id = $2), "
                        "changed AS MATERIALIZED ("
                        " SELECT c.open_time, c.updated_at FROM altml.fdw_candles_1m c "
                        " CROSS JOIN label_bounds b "
                        " WHERE c.exchange_id = 1 AND c.instrument_id = $1 AND c.is_closed "
                        " AND c.updated_at >= to_timestamp($3::bigint / 1000.0) "
                        // A candle outside these bounds cannot affect an
                        // already materialized label.  This is important while
                        // a newly listed instrument is still catching up.
                        " AND b.lo IS NOT NULL "
                        " AND c.open_time BETWEEN b.lo - make_interval(mins => $4) "
                        "                         AND b.hi + make_interval(mins => $5)) "
                        "SELECT (extract(epoch FROM min(("
                        // Keep the range lookup correlated.  A flattened
                        // range join made PostgreSQL scan every label for
                        // every changed candle on partitioned tables.  This
                        // form performs bounded PK range probes (at most 45
                        // one-minute labels per changed candle).
                        " SELECT min(l.bar_open_time) FROM altml.labels l "
                        " WHERE l.exchange_id = 1 AND l.instrument_id = $1 "
                        " AND l.timeframe = '1m' AND l.label_set_version_id = $2 "
                        " AND l.bar_open_time BETWEEN c.open_time - make_interval(mins => $5) "
                        "                         AND c.open_time + make_interval(mins => $4) "
                        " AND c.updated_at > l.computed_at))) * 1000)::bigint "
                        "FROM changed c",
                        instrument_id, set_id_, revision_audit_since_ms, spec_.warmup_minutes,
                        spec_.max_horizon);
                    if (!stale.empty() && !stale[0][0].is_null()) {
                        const int64_t audit_ms = stale[0][0].as<int64_t>();
                        if (from_ms == 0 || audit_ms < from_ms) from_ms = audit_ms;
                    }
                }

                if (force_from_first_bar) {
                    const auto first = txn.exec_params(
                        "SELECT (extract(epoch FROM min(open_time)) * 1000)::bigint "
                        "FROM altml.fdw_candles_1m WHERE exchange_id = 1 AND instrument_id = $1 "
                        "AND is_closed",
                        instrument_id);
                    if (!first.empty() && !first[0][0].is_null()) {
                        from_ms = first[0][0].as<int64_t>();
                    }
                }
            });
            if (!res) return std::unexpected(res.error());
        }
        if (from_ms == 0) return progress;

        int64_t cursor = from_ms;
        // ATR(14) causal warmup. Keep bad bars in the input so a gap or a
        // quality failure resets the chain instead of being compressed away.
        // Only a set whose labels read a causal state needs it.
        labels::AtrState atr_state;
        if (spec_.warmup_minutes > 0) {
            auto res = pool_.with_txn([&](pqxx::work& txn) {
                const auto rows = txn.exec_params(
                    "SELECT (extract(epoch FROM open_time) * 1000)::bigint, open::float8, "
                    "high::float8, low::float8, close::float8, revision, "
                    "quality_status = 'ok' FROM altml.fdw_candles_1m "
                    "WHERE exchange_id = 1 AND instrument_id = $1 AND is_closed "
                    "AND open_time < to_timestamp($2::bigint / 1000.0) "
                    "ORDER BY open_time DESC LIMIT 15",
                    instrument_id, cursor);
                std::vector<LBar> hist;
                for (const auto& r : rows) {
                    hist.push_back({r[0].as<int64_t>(), r[1].as<double>(), r[2].as<double>(),
                                    r[3].as<double>(), r[4].as<double>(), r[5].as<int64_t>(),
                                    r[6].as<bool>()});
                }
                std::reverse(hist.begin(), hist.end());
                for (const auto& bar : hist) atr_state.advance(bar);
            });
            if (!res) return std::unexpected(res.error());
        }

        // Each fetch is bounded in TIME as well as in rows. postgres_fdw does
        // not ship `ORDER BY open_time LIMIT 3000` to AltSpotter, so an
        // unbounded fetch transferred every candle from the cursor to now and
        // sorted it locally: 57 600 rows and 3.3 s per 3000-row chunk when the
        // cursor is 40 days back -- which is where a new label set starts. The
        // window holds the chunk plus one horizon of future and a margin, and
        // its bound IS shipped.
        const int64_t window_ms =
            (3000LL + static_cast<int64_t>(spec_.max_horizon) + 60LL) * 60'000LL;
        while (!ShutdownToken::instance().stop_requested() &&
               progress.rows_written < row_limit) {
            std::vector<LBar> bars;
            const int64_t window_end = cursor + window_ms;
            auto res = pool_.with_txn([&](pqxx::work& txn) {
                const auto rows = txn.exec_params(
                    "SELECT (extract(epoch FROM open_time) * 1000)::bigint, open::float8, "
                    "high::float8, low::float8, close::float8, revision, "
                    "quality_status = 'ok' "
                    "FROM altml.fdw_candles_1m WHERE exchange_id = 1 AND instrument_id = $1 "
                    "AND is_closed AND open_time >= to_timestamp($2::bigint / 1000.0) "
                    "AND open_time < to_timestamp($3::bigint / 1000.0) "
                    "ORDER BY open_time LIMIT 3000",
                    instrument_id, cursor, window_end);
                for (const auto& r : rows) {
                    bars.push_back({r[0].as<int64_t>(), r[1].as<double>(), r[2].as<double>(),
                                    r[3].as<double>(), r[4].as<double>(), r[5].as<int64_t>(),
                                    r[6].as<bool>()});
                }
            });
            if (!res) return std::unexpected(res.error());
            const auto horizon = static_cast<size_t>(spec_.max_horizon);
            const bool window_in_past =
                window_end < timeutil::now_ms() - (static_cast<int64_t>(horizon) + 1) * 60'000LL;
            if (bars.size() <= horizon) {
                // A window this sparse contains no bar whose whole future it
                // holds. If it lies entirely in the past it is a hole in the
                // candle history: step over it, keeping its last horizon so
                // the bars there are fetched again with their futures. Only a
                // window that reaches the present means "caught up".
                if (window_in_past) {
                    cursor = window_end - (static_cast<int64_t>(horizon) + 1) * 60'000LL;
                    continue;
                }
                break; // nothing labelable yet
            }

            std::vector<LabelRow> out;
            const size_t labelable = bars.size() - horizon;
            const size_t take = std::min<size_t>(
                labelable, static_cast<size_t>(row_limit - progress.rows_written));
            out.reserve(take);
            // Label bars [0 .. size-kMaxHorizon-1]; the rest lack full future.
            // A bad anchor is persisted only as an all-invalid bitmap tombstone
            // so incremental progress cannot loop forever at that minute. It
            // carries no usable label and the dataset source gate excludes it.
            for (size_t t = 0; t < take; ++t) {
                if (spec_.core) {
                    atr_state.advance(bars[t]);
                    out.push_back(compute_labels(instrument_id, bars, t, atr_state));
                } else {
                    out.push_back(compute_econ_labels(instrument_id, bars, t));
                }
            }
            auto w = write_labels(out);
            if (!w) return std::unexpected(w.error());
            if (*w > 0) {
                if (progress.range_from_ms == 0) progress.range_from_ms = out.front().bar_open_ms;
                progress.range_to_ms = out.back().bar_open_ms;
                progress.rows_written += *w;
            }
            // advance: continue after the last LABELED bar; ATR window continuity
            // is preserved because the next chunk starts at the next bar.
            cursor = bars[take].open_ms;
            if (take < labelable ||
                (progress.rows_written >= row_limit && bars.size() == 3000)) {
                progress.caught_up = false;
                break;
            }
            // Fewer rows than the chunk: either a gap inside a past window
            // (keep going) or the present (stop until the next cycle).
            if (bars.size() < 3000 && !window_in_past) break;
        }
        return progress;
    }

    void record_run(const LabelProgress& progress, const std::string& status,
                    const std::string& error, int64_t started_ms) {
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            txn.exec_params(
                "INSERT INTO altml.label_computation_runs (service, exchange_id, instrument_id, "
                "range_from, range_to, rows_written, status, error, started_at, finished_at) "
                "VALUES ('ml-label-builder-service', 1, NULL, "
                "to_timestamp(NULLIF($1::bigint, 0) / 1000.0), "
                "to_timestamp(NULLIF($2::bigint, 0) / 1000.0), $3, $4, NULLIF($5, ''), "
                "to_timestamp($6::bigint / 1000.0), now())",
                progress.range_from_ms, progress.range_to_ms, progress.rows_written, status,
                error, started_ms);
        });
        if (!res) {
            log::error("label_run_record_failed", {{"message", res.error().message}});
        }
    }

    Result<bool> initial_atr_repair_needed() {
        bool needed = true;
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            const auto rows = txn.exec_params(
                "SELECT EXISTS (SELECT 1 FROM altml.label_computation_runs "
                "WHERE service = $1 AND status = 'completed')",
                std::string(kInitialAtrRepairRun));
            needed = !rows[0][0].as<bool>();
        });
        if (!res) return std::unexpected(res.error());
        return needed;
    }

    Result<int64_t> last_revision_audit_ms(int64_t fallback_ms) {
        int64_t out = fallback_ms;
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            const auto rows = txn.exec_params(
                "SELECT (extract(epoch FROM max(finished_at)) * 1000)::bigint "
                "FROM altml.label_computation_runs WHERE service = $1 AND status = 'completed'",
                audit_run_name());
            if (!rows.empty() && !rows[0][0].is_null()) out = rows[0][0].as<int64_t>();
        });
        if (!res) return std::unexpected(res.error());
        return out;
    }

    Result<void> record_revision_audit(int64_t started_ms) {
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            txn.exec_params(
                "INSERT INTO altml.label_computation_runs (service, exchange_id, instrument_id, "
                "rows_written, status, started_at, finished_at) "
                "VALUES ($1, 1, NULL, 0, 'completed', "
                "to_timestamp($2::bigint / 1000.0), now())",
                audit_run_name(), started_ms);
        });
        if (!res) return std::unexpected(res.error());
        return {};
    }

    Result<void> record_initial_atr_repair(int64_t rows_written, int64_t started_ms) {
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            txn.exec_params(
                "INSERT INTO altml.label_computation_runs (service, exchange_id, instrument_id, "
                "rows_written, status, started_at, finished_at) "
                "VALUES ($1, 1, NULL, $2, 'completed', "
                "to_timestamp($3::bigint / 1000.0), now())",
                std::string(kInitialAtrRepairRun), rows_written, started_ms);
            txn.exec_params(
                "UPDATE altml.label_set_versions SET builder_version = $2 WHERE id = $1",
                set_id_, spec_.builder_version);
        });
        if (!res) return std::unexpected(res.error());
        return {};
    }

    [[nodiscard]] const LabelSetSpec& spec() const { return spec_; }

    // Each set keeps its own audit watermark: a revision audit completed for
    // one set says nothing about the other's labels.
    [[nodiscard]] std::string audit_run_name() const {
        return spec_.core ? std::string(kRevisionAuditRun)
                          : std::string(kRevisionAuditRun) + ":" + spec_.name;
    }

private:
    // econ-labels: the path outcome over each horizon and its class against
    // the all-in cost. A horizon whose path is incomplete or touches a bad
    // bar has no outcome -- all six of its labels stay invalid together.
    LabelRow compute_econ_labels(int64_t instrument_id, const std::vector<LBar>& bars, size_t t) {
        LabelRow row;
        row.instrument_id = instrument_id;
        row.bar_open_ms = bars[t].open_ms;
        row.bar_revision = bars[t].revision;
        row.values.assign(spec_.defs.size(), 0.0F);
        row.validity.assign((spec_.defs.size() + 7) / 8, 0);
        size_t idx = 0;
        const auto put = [&](double v, bool ok) {
            if (ok && std::isfinite(v)) {
                row.values[idx] = static_cast<float>(v);
                row.validity[idx / 8] |= static_cast<uint8_t>(1U << (idx % 8));
            }
            ++idx;
        };
        for (const auto& h : econ::kHorizons) {
            const bool ok = labels::source_range_valid(bars, t, h.minutes);
            econ::PathOutcome o;
            if (ok) o = econ::path_outcome(bars, t, h.minutes);
            put(o.log_return, ok);
            put(o.mfe_long, ok);
            put(o.mae_long, ok);
            put(static_cast<double>(o.minutes_to_mfe), ok);
            put(static_cast<double>(o.minutes_to_mae), ok);
            put(static_cast<double>(econ::econ_class(o.log_return, econ_cost_)), ok);
        }
        return row;
    }

    LabelRow compute_labels(int64_t instrument_id, const std::vector<LBar>& bars, size_t t,
                            const labels::AtrState& atr_state) {
        const auto& defs = spec_.defs;
        LabelRow row;
        row.instrument_id = instrument_id;
        row.bar_open_ms = bars[t].open_ms;
        row.bar_revision = bars[t].revision;
        row.values.assign(defs.size(), 0.0F);
        row.validity.assign((defs.size() + 7) / 8, 0);
        size_t idx = 0;
        const auto put = [&](double v, bool ok) {
            if (ok && std::isfinite(v)) {
                row.values[idx] = static_cast<float>(v);
                row.validity[idx / 8] |= static_cast<uint8_t>(1U << (idx % 8));
            }
            ++idx;
        };
        const double c0 = bars[t].close;
        const auto future_ret = [&](int h) { return std::log(bars[t + h].close / c0); };
        const auto source_ok = [&](int h) {
            return labels::source_range_valid(bars, t, h);
        };

        for (const int h : {1, 3, 5, 10, 15, 30}) put(future_ret(h), source_ok(h));
        for (const int h : {1, 5, 15, 30}) put(future_ret(h) - round_trip_fee_, source_ok(h));
        for (const int h : {5, 15, 30}) {
            double mfe = 0, mae = 0;
            for (int k = 1; k <= h; ++k) {
                mfe = std::max(mfe, bars[t + k].high / c0 - 1.0);
                mae = std::min(mae, bars[t + k].low / c0 - 1.0);
            }
            put(mfe, source_ok(h));
            put(mae, source_ok(h));
        }
        for (const int h : {5, 15, 30}) {
            double acc = 0;
            for (int k = 1; k <= h; ++k) {
                const double r = std::log(bars[t + k].close / bars[t + k - 1].close);
                acc += r * r;
            }
            put(std::sqrt(acc / h), source_ok(h));
        }
        for (const int h : {5, 15, 30}) {
            put(bars[t + h].close > c0 ? 1.0 : (bars[t + h].close < c0 ? -1.0 : 0.0),
                source_ok(h));
        }
        // barrier labels
        const double atr = atr_state.value();
        const bool atr_ok = atr_state.ready() && atr > 0 && source_ok(0);
        const int kMaxHorizon = spec_.max_horizon;
        const bool barrier_source_ok = source_ok(kMaxHorizon);
        double target = c0 + kBarrierAtrMult * atr;
        double stop = c0 - kBarrierAtrMult * atr;
        int hit_target = -1, hit_stop = -1;
        if (atr_ok && barrier_source_ok) {
            for (int k = 1; k <= kMaxHorizon; ++k) {
                const bool tgt = bars[t + k].high >= target;
                const bool stp = bars[t + k].low <= stop;
                if (tgt && stp) {
                    // Ambiguous bar: conservative rule — stop counted first.
                    if (hit_stop < 0) hit_stop = k;
                    break;
                }
                if (stp && hit_stop < 0) {
                    hit_stop = k;
                    break;
                }
                if (tgt && hit_target < 0) {
                    hit_target = k;
                    break;
                }
            }
        }
        put(hit_target > 0 ? 1.0 : 0.0, atr_ok && barrier_source_ok);
        put(hit_target > 0 ? static_cast<double>(hit_target) : 0.0,
            atr_ok && barrier_source_ok && hit_target > 0);
        put(hit_stop > 0 ? static_cast<double>(hit_stop) : 0.0,
            atr_ok && barrier_source_ok && hit_stop > 0);
        return row;
    }

    Result<int64_t> write_labels(const std::vector<LabelRow>& rows) {
        if (rows.empty()) return 0;
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            txn.exec("SET LOCAL TIME ZONE 'UTC'");
            for (size_t off = 0; off < rows.size(); off += 500) {
                const size_t end = std::min(rows.size(), off + 500);
                std::string sql =
                    "INSERT INTO altml.labels (exchange_id, instrument_id, timeframe, "
                    "bar_open_time, bar_revision, label_set_version_id, values, validity_bitmap) "
                    "VALUES ";
                bool first = true;
                char buf[32];
                for (size_t i = off; i < end; ++i) {
                    const auto& r = rows[i];
                    sql += first ? "(" : ",(";
                    first = false;
                    sql += "1," + std::to_string(r.instrument_id) + ",'1m',to_timestamp(" +
                           std::to_string(r.bar_open_ms) + "::bigint / 1000.0)," +
                           std::to_string(r.bar_revision) + "," + std::to_string(set_id_) + ",'{";
                    for (size_t j = 0; j < r.values.size(); ++j) {
                        if (j != 0) sql += ',';
                        std::snprintf(buf, sizeof(buf), "%.9g",
                                      static_cast<double>(r.values[j]));
                        sql += buf;
                    }
                    sql += "}'::real[],'\\x";
                    static constexpr char h[] = "0123456789abcdef";
                    for (const uint8_t b : r.validity) {
                        sql += h[b >> 4];
                        sql += h[b & 0x0F];
                    }
                    sql += "'::bytea)";
                }
                sql += " ON CONFLICT (exchange_id, instrument_id, timeframe, bar_open_time, "
                       "bar_revision, label_set_version_id) DO UPDATE SET "
                       "values = EXCLUDED.values, validity_bitmap = EXCLUDED.validity_bitmap, "
                       "computed_at = now()";
                txn.exec(sql);
            }
        });
        if (!res) return std::unexpected(res.error());
        return static_cast<int64_t>(rows.size());
    }

    persistence::PgPool& pool_;
    const LabelSetSpec& spec_;
    double round_trip_fee_;
    double econ_cost_;
    int fee_model_version_;
    std::string schema_hash_;
    int64_t set_id_ = 0;
};

} // namespace

struct LabelBuilderComponent::Impl {
    std::optional<persistence::PgPool> pool;
    std::optional<LabelBuilder> builder;        // core-labels
    std::optional<LabelBuilder> econ_builder;   // econ-labels
    int64_t cycle_sec = 300;
    int64_t max_bars_per_cycle = 5000;
    int64_t econ_max_bars_per_cycle = 20000;
    int64_t revision_audit_sec = 3600;
    bool initial_atr_repair_pending = false;
    int64_t last_revision_audit_ms = 0;
    int64_t econ_last_revision_audit_ms = 0;
};

LabelBuilderComponent::LabelBuilderComponent(const config::RuntimeConfig& rt)
    : rt_(rt), impl_(std::make_unique<Impl>()) {}

LabelBuilderComponent::~LabelBuilderComponent() = default;

Result<void> LabelBuilderComponent::init() {
    const YAML::Node section = rt_.root["label_builder"];
    impl_->cycle_sec = config::opt_int(section, "cycle_interval_sec", 300);
    impl_->max_bars_per_cycle = config::opt_int(section, "max_bars_per_cycle", 5000);
    // econ-labels reads 240 minutes of future per anchor and starts life 41
    // days behind; at the core budget its first backfill would take most of a
    // day. Its own budget, stated separately so neither starves the other.
    impl_->econ_max_bars_per_cycle =
        config::opt_int(section, "econ_max_bars_per_cycle", impl_->econ_max_bars_per_cycle);
    const int fee_model_version =
        static_cast<int>(config::opt_int(section, "fee_model_version", 1));
    impl_->revision_audit_sec = config::opt_int(section, "revision_audit_interval_sec", 3600);
    if (impl_->cycle_sec <= 0 || impl_->max_bars_per_cycle <= 0 ||
        impl_->econ_max_bars_per_cycle <= 0 || fee_model_version <= 0 ||
        impl_->revision_audit_sec <= 0) {
        return fail(ErrorCode::config_error, "invalid label_builder configuration");
    }
    impl_->pool.emplace(rt_.common.postgres_dsn,
                        static_cast<size_t>(config::opt_int(section, "postgres_pool_size", 2)));
    auto& pool = *impl_->pool;

    double taker_fee = 0;
    double slippage_bps = 0;
    bool fee_model_found = false;
    {
        auto res = pool.with_txn([&](pqxx::work& txn) {
            const auto r = txn.exec_params(
                "SELECT taker_fee::float8, slippage_estimate_bps::float8 FROM altml.fee_models "
                "WHERE version = $1",
                fee_model_version);
            if (!r.empty()) {
                taker_fee = r[0][0].as<double>();
                slippage_bps = r[0][1].as<double>();
                fee_model_found = true;
            }
        });
        if (!res) {
            return fail(ErrorCode::database_error,
                        "postgres unavailable: " + res.error().message);
        }
    }
    if (!fee_model_found || taker_fee < 0 || slippage_bps < 0) {
        return fail(ErrorCode::config_error,
                    "fee model unavailable: version " + std::to_string(fee_model_version));
    }

    impl_->builder.emplace(pool, core_spec(), taker_fee, slippage_bps, fee_model_version);
    impl_->econ_builder.emplace(pool, econ_spec(), taker_fee, slippage_bps, fee_model_version);
    auto& builder = *impl_->builder;
    if (auto r = ensure_data_partitions(pool, timeutil::now_ms()); !r) {
        return fail(r.error().code, "partition setup failed: " + r.error().message);
    }
    if (auto r = refresh_foreign_table_statistics(pool); !r) {
        return fail(r.error().code, "foreign table statistics failed: " + r.error().message);
    }
    for (auto* b : {&*impl_->builder, &*impl_->econ_builder}) {
        if (auto r = b->ensure_set(); !r) {
            return fail(r.error().code, "label set " + b->spec().name +
                                            " init failed: " + r.error().message);
        }
    }
    auto repair_needed = builder.initial_atr_repair_needed();
    if (!repair_needed) {
        return fail(repair_needed.error().code,
                    "label repair state failed: " + repair_needed.error().message);
    }
    impl_->initial_atr_repair_pending = *repair_needed;

    const int64_t startup_ms = timeutil::now_ms();
    for (auto [b, slot] : {std::pair{&*impl_->builder, &impl_->last_revision_audit_ms},
                           std::pair{&*impl_->econ_builder, &impl_->econ_last_revision_audit_ms}}) {
        auto persisted_audit = b->last_revision_audit_ms(startup_ms - 2 * 60 * 60 * 1000);
        if (!persisted_audit) {
            return fail(persisted_audit.error().code,
                        "label revision audit state failed: " + persisted_audit.error().message);
        }
        *slot = *persisted_audit;
    }

    set_component_status(name(), "labeling closed 1m bars");
    return {};
}

namespace {

// One pass of one label set over every instrument, bounded by `budget` rows.
// Returns whether the set completed its revision audit this pass.
struct SetPass {
    LabelProgress progress;
    bool failed = false;
    std::string error;
};

SetPass run_set_pass(LabelBuilder& builder, const std::vector<int64_t>& instruments,
                     int64_t budget, bool audit_revisions, int64_t& last_audit_ms,
                     int64_t started) {
    auto& shutdown = ShutdownToken::instance();
    SetPass pass;
    bool audit_complete = true;
    size_t processed = 0;
    for (const int64_t iid : instruments) {
        if (shutdown.stop_requested()) break;
        const int64_t remaining = budget - pass.progress.rows_written;
        if (remaining <= 0) {
            audit_complete = false;
            break;
        }
        auto progress = builder.process_instrument(iid, remaining,
                                                   audit_revisions ? last_audit_ms : 0);
        if (!progress) {
            pass.failed = true;
            if (pass.error.empty()) pass.error = progress.error().message;
            log::error("label_instrument_failed",
                       {{"label_set", builder.spec().name},
                        {"instrument_id", iid},
                        {"message", progress.error().message}});
            continue;
        }
        ++processed;
        if (!progress->caught_up) audit_complete = false;
        if (progress->rows_written > 0) {
            if (pass.progress.range_from_ms == 0 ||
                progress->range_from_ms < pass.progress.range_from_ms) {
                pass.progress.range_from_ms = progress->range_from_ms;
            }
            pass.progress.range_to_ms = std::max(pass.progress.range_to_ms, progress->range_to_ms);
            pass.progress.rows_written += progress->rows_written;
        }
    }
    if (processed != instruments.size()) audit_complete = false;
    pass.progress.caught_up = audit_complete;
    if (audit_revisions && !pass.failed && audit_complete) {
        auto marker = builder.record_revision_audit(started);
        if (!marker) {
            pass.failed = true;
            pass.error = marker.error().message;
            log::error("label_revision_audit_record_failed",
                       {{"label_set", builder.spec().name}, {"message", marker.error().message}});
        } else {
            last_audit_ms = started;
        }
    }
    return pass;
}

} // namespace

void LabelBuilderComponent::run() {
    auto& shutdown = ShutdownToken::instance();
    auto& pool = *impl_->pool;
    auto& builder = *impl_->builder;
    auto& econ_builder = *impl_->econ_builder;
    bool initial_atr_repair_pending = impl_->initial_atr_repair_pending;
    int64_t last_revision_audit_ms = impl_->last_revision_audit_ms;
    int64_t econ_last_revision_audit_ms = impl_->econ_last_revision_audit_ms;
    int64_t last_partition_check_ms = 0;
    const int64_t revision_audit_sec = impl_->revision_audit_sec;

    while (!shutdown.stop_requested()) {
        const int64_t started = timeutil::now_ms();
        if (started - last_partition_check_ms >= 24LL * 3600 * 1000) {
            if (auto r = ensure_data_partitions(pool, started); !r) {
                log::error("partition_maintenance_failed", {{"message", r.error().message}});
            } else {
                last_partition_check_ms = started;
            }
            if (auto r = refresh_foreign_table_statistics(pool); !r) {
                log::error("foreign_table_statistics_failed", {{"message", r.error().message}});
            }
        }
        std::vector<int64_t> instruments;
        auto res = pool.with_txn([&](pqxx::work& txn) {
            const auto rows = txn.exec_params(
                "SELECT ei.instrument_id FROM altml.fdw_exchange_instruments ei "
                "JOIN altml.fdw_exchanges e ON e.id = ei.exchange_id "
                "WHERE e.code = $1 AND ei.is_whitelisted AND ei.status = 'active' "
                "ORDER BY ei.instrument_id",
                rt_.project.exchange);
            for (const auto& r : rows) instruments.push_back(r[0].as<int64_t>());
        });
        LabelProgress cycle_progress;
        bool cycle_failed = false;
        std::string cycle_error;
        if (res) {
            if (initial_atr_repair_pending) {
                int64_t repaired_rows = 0;
                bool repair_failed = false;
                for (const int64_t iid : instruments) {
                    if (shutdown.stop_requested()) {
                        repair_failed = true;
                        cycle_error = "shutdown during initial ATR repair";
                        break;
                    }
                    // Rebuild the first ATR warmup boundary once for this
                    // immutable builder version. This also makes interrupted
                    // upgrades deterministic before normal catch-up resumes.
                    auto repaired = builder.process_instrument(iid, 15, 0, true);
                    if (!repaired) {
                        repair_failed = true;
                        cycle_error = repaired.error().message;
                        log::error("initial_atr_repair_failed",
                                   {{"instrument_id", iid},
                                    {"message", repaired.error().message}});
                        break;
                    }
                    repaired_rows += repaired->rows_written;
                }
                if (!repair_failed) {
                    auto marker = builder.record_initial_atr_repair(repaired_rows, started);
                    if (!marker) {
                        repair_failed = true;
                        cycle_error = marker.error().message;
                    } else {
                        initial_atr_repair_pending = false;
                        log::info("initial_atr_repair_completed", {{"rows", repaired_rows}});
                    }
                }
                if (repair_failed) cycle_failed = true;
            }

            const bool audit_core = started - last_revision_audit_ms >= revision_audit_sec * 1000;
            const bool audit_econ =
                started - econ_last_revision_audit_ms >= revision_audit_sec * 1000;
            const auto core = run_set_pass(builder, instruments, impl_->max_bars_per_cycle,
                                           audit_core, last_revision_audit_ms, started);
            const auto econ_pass =
                run_set_pass(econ_builder, instruments, impl_->econ_max_bars_per_cycle, audit_econ,
                             econ_last_revision_audit_ms, started);
            for (const auto* pass : {&core, &econ_pass}) {
                if (pass->failed) {
                    cycle_failed = true;
                    if (cycle_error.empty()) cycle_error = pass->error;
                }
                if (pass->progress.rows_written > 0) {
                    if (cycle_progress.range_from_ms == 0 ||
                        pass->progress.range_from_ms < cycle_progress.range_from_ms) {
                        cycle_progress.range_from_ms = pass->progress.range_from_ms;
                    }
                    cycle_progress.range_to_ms =
                        std::max(cycle_progress.range_to_ms, pass->progress.range_to_ms);
                    cycle_progress.rows_written += pass->progress.rows_written;
                }
            }
            if (cycle_progress.rows_written > 0) {
                log::info("labels_written", {{"rows", cycle_progress.rows_written},
                                             {"core_rows", core.progress.rows_written},
                                             {"econ_rows", econ_pass.progress.rows_written},
                                             {"econ_caught_up", econ_pass.progress.caught_up},
                                             {"duration_ms", timeutil::now_ms() - started}});
            }
        } else {
            cycle_failed = true;
            cycle_error = res.error().message;
            log::error("label_cycle_failed", {{"message", res.error().message}});
        }
        if (cycle_progress.rows_written > 0 || cycle_failed) {
            builder.record_run(cycle_progress, cycle_failed ? "failed" : "completed",
                               cycle_error, started);
        }
        if (shutdown.wait_for(std::chrono::seconds(impl_->cycle_sec))) break;
    }
}

} // namespace altml
