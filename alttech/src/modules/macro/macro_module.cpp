#include "alttech/modules/macro.hpp"

#include "alttech/application/module_supervisor.hpp"
#include "alttech/common/shutdown.hpp"
#include "alttech/logging/logger.hpp"
#include "alttech/messaging/codec.hpp"
#include "alttech/messaging/nats_client.hpp"
#include "alttech/messaging/topology.hpp"
#include "alttech/persistence/pg.hpp"

#include "macro_availability.hpp"
#include "macro_compute.hpp"
#include "macro_features.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace alttech::modules {
namespace {

using namespace alttech::modules::macro;

constexpr const char* kModuleName = "macro";
constexpr int64_t kMinute = 60'000;
constexpr int64_t kDay = 86'400'000;

// Two days of lead-in before the first emitted minute. The longest lookback is
// a one-day return and the longest rolling window is 1440 minutes, so a chunk
// that began cold would publish a day of blank validity bits at every boundary.
constexpr int64_t kWarmupMs = 2 * kDay;

struct Params {
    int64_t history_start_ms = 0;
    int cycle_interval_sec = 60;
    int live_lookback_minutes = 240;
    int64_t liveness_budget_ms = 900'000;
};

int64_t iso_to_ms(const std::string& iso) {
    std::tm tm{};
    if (iso.size() < 10) return 0;
    tm.tm_year = std::atoi(iso.substr(0, 4).c_str()) - 1900;
    tm.tm_mon = std::atoi(iso.substr(5, 2).c_str()) - 1;
    tm.tm_mday = std::atoi(iso.substr(8, 2).c_str());
    return static_cast<int64_t>(timegm(&tm)) * 1000;
}

// --- reading AltMacro through the read-only foreign schema ------------------

std::vector<IntradayPoint> load_intraday(pqxx::work& txn, const std::string& code,
                                          int64_t from_ms, int64_t to_ms) {
    // Bid and ask are separate series in AltMacro, as they are separate files
    // at the source. They are fetched apart and merged here rather than joined
    // remotely: a self-join across postgres_fdw on 400k rows is a remote
    // round-trip per row.
    auto fetch = [&](const char* basis) {
        std::map<int64_t, std::pair<double, bool>> out;
        const auto rows = txn.exec_params(
            "SELECT EXTRACT(EPOCH FROM b.event_time)*1000, b.close, b.quality_status "
            "FROM altmacro_ref.macro_price_bars b "
            "JOIN altmacro_ref.macro_instruments i ON i.id = b.instrument_id "
            "WHERE i.code = $1 AND b.timeframe = '1m' AND b.price_basis = $2 "
            "  AND b.close IS NOT NULL "
            "  AND b.event_time >= to_timestamp($3::bigint/1000.0) "
            "  AND b.event_time <  to_timestamp($4::bigint/1000.0) "
            "ORDER BY b.event_time",
            code, std::string(basis), from_ms, to_ms);
        for (const auto& r : rows) {
            out.emplace(static_cast<int64_t>(r[0].as<double>()),
                        std::make_pair(r[1].as<double>(),
                                       r[2].as<std::string>() == "VALID"));
        }
        return out;
    };

    const auto bid = fetch("bid");
    const auto ask = fetch("ask");

    std::vector<IntradayPoint> out;
    out.reserve(bid.size());
    for (const auto& [t, b] : bid) {
        const auto a = ask.find(t);
        if (a == ask.end()) continue;   // one side missing: no usable mid
        IntradayPoint p;
        p.t_ms = t;
        p.mid = (b.first + a->second.first) / 2.0;
        p.spread_bps = p.mid > 0.0 ? (a->second.first - b.first) / p.mid * 10'000.0 : 0.0;
        // Traded only when BOTH sides were real quotes. AltMacro marks a minute
        // STALE when the feed repeated the last price at zero volume, and a
        // return computed across such a stretch measures the carry-forward
        // rather than the market.
        p.traded = b.second && a->second.second;
        out.push_back(p);
    }
    return out;
}

std::vector<DailyPoint> load_daily(pqxx::work& txn, const std::string& code) {
    std::vector<DailyPoint> out;
    const auto rows = txn.exec_params(
        "SELECT EXTRACT(EPOCH FROM date_trunc('day', v.event_time))*1000, v.value "
        "FROM altmacro_ref.macro_reference_values v "
        "JOIN altmacro_ref.macro_instruments i ON i.id = v.instrument_id "
        "WHERE i.code = $1 AND v.quality_status = 'VALID' AND v.value IS NOT NULL "
        "ORDER BY v.event_time",
        code);
    out.reserve(rows.size());
    for (const auto& r : rows) {
        out.push_back(DailyPoint{static_cast<int64_t>(r[0].as<double>()), r[1].as<double>()});
    }
    return out;
}

std::vector<DailyPoint> load_cnh_daily_mid(pqxx::work& txn) {
    // One offshore mid per day at 01:30 UTC -- fifteen minutes after the PBOC
    // fix is published, when both sides of the gap are fresh. Only real quotes:
    // a carried-forward weekend price would be Friday's mid wearing Saturday's
    // date.
    std::vector<DailyPoint> out;
    const auto rows = txn.exec(
        "SELECT EXTRACT(EPOCH FROM date_trunc('day', b.event_time))*1000, "
        "       (b.close + a.close) / 2.0 "
        "FROM altmacro_ref.macro_price_bars b "
        "JOIN altmacro_ref.macro_price_bars a "
        "  ON a.instrument_id = b.instrument_id AND a.timeframe = b.timeframe "
        " AND a.event_time = b.event_time AND a.price_basis = 'ask' "
        "JOIN altmacro_ref.macro_instruments i ON i.id = b.instrument_id "
        "WHERE i.code = 'FX.USDCNH' AND b.timeframe = '1m' AND b.price_basis = 'bid' "
        "  AND b.quality_status = 'VALID' AND a.quality_status = 'VALID' "
        "  AND b.event_time = date_trunc('day', b.event_time) + interval '90 minutes' "
        "ORDER BY b.event_time");
    out.reserve(rows.size());
    for (const auto& r : rows) {
        out.push_back(DailyPoint{static_cast<int64_t>(r[0].as<double>()), r[1].as<double>()});
    }
    return out;
}

MacroInputs load_inputs(persistence::PgPool& pool, int64_t from_ms, int64_t to_ms) {
    MacroInputs in;
    auto r = pool.with_txn([&](pqxx::work& txn) {
        in.cnh_daily_mid = load_cnh_daily_mid(txn);
        in.usdcnh = load_intraday(txn, "FX.USDCNH", from_ms, to_ms);
        in.eurusd = load_intraday(txn, "FX.EURUSD", from_ms, to_ms);
        in.usdjpy = load_intraday(txn, "FX.USDJPY", from_ms, to_ms);
        in.xauusd = load_intraday(txn, "METAL.XAUUSD", from_ms, to_ms);
        // Daily series are a few hundred rows each and the rolling windows need
        // their whole history, so they are read whole rather than chunked.
        in.fixing = load_daily(txn, "REF.USDCNY.PBOC");
        in.h10_cny = load_daily(txn, "REF.USDCNY.FED");
        in.gbpusd = load_daily(txn, "REF.GBPUSD.FED");
        in.audusd = load_daily(txn, "REF.AUDUSD.FED");
        in.usdkrw = load_daily(txn, "REF.USDKRW.FED");
        in.vix = load_daily(txn, "IDX.VIX");
        in.spx = load_daily(txn, "IDX.SPX");
        in.ndx = load_daily(txn, "IDX.NDX");
        in.us2y = load_daily(txn, "RATE.US.2Y");
        in.us10y = load_daily(txn, "RATE.US.10Y");
        in.us10y_real = load_daily(txn, "RATE.US.10Y.REAL");
    });
    if (!r) {
        log::error("macro_inputs_load_failed", {{"error", r.error().message}});
    }
    return in;
}

// --- feature set registration -----------------------------------------------

Result<int64_t> ensure_feature_set(persistence::PgPool& pool) {
    int64_t set_id = 0;
    const std::string hash = macro_schema_fingerprint();
    auto res = pool.with_txn([&](pqxx::work& txn) {
        const auto rows = txn.exec_params(
            "SELECT id, schema_hash FROM alttech.feature_set_versions "
            "WHERE name = $1 AND version = $2",
            std::string(kMacroFeatureSetName), kMacroFeatureSetVersion);
        if (!rows.empty()) {
            set_id = rows[0][0].as<int64_t>();
            const auto stored = rows[0][1].as<std::string>();
            if (stored != hash) {
                // The catalog changed under a frozen version. Every stored
                // vector's positions would now mean something else, and nothing
                // downstream could tell. Refuse rather than write into it.
                throw std::runtime_error(
                    "macro-core v" + std::to_string(kMacroFeatureSetVersion) +
                    " is frozen with a different column list (stored " + stored.substr(0, 12) +
                    ", built " + hash.substr(0, 12) + "): bump the version instead of editing");
            }
            return;
        }
        const auto ins = txn.exec_params(
            "INSERT INTO alttech.feature_set_versions "
            "(name, version, scope, profile, engine_version, method_version, schema_hash, frozen) "
            "VALUES ($1, $2, 'GLOBAL', 'macro_core', $3, $4, $5, true) RETURNING id",
            std::string(kMacroFeatureSetName), kMacroFeatureSetVersion,
            std::string("alttech-macro-1"), std::string(kMacroMethodVersion), hash);
        set_id = ins[0][0].as<int64_t>();
        const auto& names = macro_feature_names();
        for (size_t i = 0; i < names.size(); ++i) {
            const auto def = txn.exec_params(
                "INSERT INTO alttech.feature_definitions (name, family, scope) "
                "VALUES ($1, 'macro', 'GLOBAL') "
                "ON CONFLICT (name) DO UPDATE SET family = EXCLUDED.family RETURNING id",
                names[i]);
            const auto ps = txn.exec_params(
                "INSERT INTO alttech.feature_parameter_sets (definition_id, parameters) "
                "VALUES ($1, '{}') ON CONFLICT (definition_id, parameters) "
                "DO UPDATE SET parameters = EXCLUDED.parameters RETURNING id",
                def[0][0].as<int64_t>());
            txn.exec_params(
                "INSERT INTO alttech.feature_set_members "
                "(feature_set_version_id, position, parameter_set_id, output_name) "
                "VALUES ($1, $2, $3, $4)",
                set_id, static_cast<int>(i), ps[0][0].as<int64_t>(), names[i]);
        }
    });
    if (!res) return std::unexpected(res.error());
    return set_id;
}

// --- storage -----------------------------------------------------------------

std::string real_array_literal(const std::vector<float>& v) {
    std::string out = "{";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i != 0) out.push_back(',');
        out += std::to_string(v[i]);
    }
    out.push_back('}');
    return out;
}

const char* status_name(VectorStatus s) {
    switch (s) {
        case VectorStatus::warming: return "WARMING";
        case VectorStatus::valid: return "VALID";
        case VectorStatus::incomplete_input: return "INCOMPLETE_INPUT";
    }
    return "INVALID";
}

// Start of the oldest macro_feature_vectors partition plus a day; 0 when the
// table has none (the backfill then keeps history_start, and the partition
// ensure at start-up is what has to fix that).
int64_t partition_floor_ms(persistence::PgPool& pool) {
    int64_t floor = 0;
    auto r = pool.with_txn([&](pqxx::work& txn) {
        const auto row = txn.exec(
            "SELECT (extract(epoch FROM min(lower_bound) + interval '1 day') * 1000)::bigint "
            "FROM alttech.partition_bounds('alttech.macro_feature_vectors'::regclass)");
        if (!row[0][0].is_null()) floor = row[0][0].as<int64_t>();
    });
    if (!r) log::warn("macro_partition_floor_unknown", {{"error", r.error().message}});
    return floor;
}

Result<int64_t> store(persistence::PgPool& pool, int64_t set_id,
                      const std::vector<MacroVector>& vectors) {
    if (vectors.empty()) return int64_t{0};
    int64_t written = 0;
    auto r = pool.with_txn([&](pqxx::work& txn) {
        for (const auto& v : vectors) {
            int valid_count = 0;
            for (size_t i = 0; i < v.values.size(); ++i) {
                if (v.is_valid(i)) ++valid_count;
            }
            const std::basic_string<std::byte> bitmap(
                reinterpret_cast<const std::byte*>(v.validity.data()), v.validity.size());
            const auto res = txn.exec_params(
                "INSERT INTO alttech.macro_feature_vectors "
                "(timeframe, bar_open_time, feature_set_version_id, method_version, "
                " values, validity_bitmap, status, valid_count) "
                "VALUES ('1m', to_timestamp($1::bigint/1000.0), $2, $3, $4::real[], $5, $6, $7) "
                // Recomputation is expected: a daily figure published late
                // changes the block for minutes already written. The row moves
                // forward, never backwards -- a recompute that sees LESS than
                // what is stored is a degraded read, not an improvement.
                //
                // A row from an older method is the one exception: when the
                // computation itself changed, the new result replaces the old
                // even where it holds fewer valid values, because "fewer" is
                // then the corrected answer (a stricter publication lag
                // withholds figures the old method handed out early).
                //
                // computed_at is when the stored values became knowable, and
                // point-in-time readers admit a row only if it was computed by
                // their T0. A same-method revision -- a late figure filling a
                // minute already written -- is new knowledge and moves it to
                // now(), as every other AltTech table does on revision; left at
                // the first write, the revised values looked as if they had
                // been known all along. A method change is also a later
                // computation, even with identical values. Never backdate it.
                // An identical same-method recompute touches nothing. This is
                // prospective writer behavior; historical rows are not repaired.
                "ON CONFLICT (timeframe, bar_open_time, feature_set_version_id) DO UPDATE SET "
                "  values = EXCLUDED.values, validity_bitmap = EXCLUDED.validity_bitmap, "
                "  status = EXCLUDED.status, valid_count = EXCLUDED.valid_count, "
                "  method_version = EXCLUDED.method_version, updated_at = now(), "
                "  computed_at = now() "
                "WHERE (EXCLUDED.method_version <> alttech.macro_feature_vectors.method_version "
                "       OR EXCLUDED.valid_count >= alttech.macro_feature_vectors.valid_count) "
                "  AND (EXCLUDED.method_version <> alttech.macro_feature_vectors.method_version "
                "       OR EXCLUDED.values IS DISTINCT FROM alttech.macro_feature_vectors.values "
                "       OR EXCLUDED.validity_bitmap IS DISTINCT FROM "
                "          alttech.macro_feature_vectors.validity_bitmap "
                "       OR EXCLUDED.status IS DISTINCT FROM alttech.macro_feature_vectors.status)",
                v.t_ms, set_id, std::string(kMacroMethodVersion),
                real_array_literal(v.values), bitmap, std::string(status_name(v.status)),
                valid_count);
            written += res.affected_rows();
        }
    });
    if (!r) return std::unexpected(r.error());
    return written;
}

// One Envelope per minute on the feature namespace, shaped exactly like the
// primary venue's own vector (FeatureVector, tag 30) so AltML's existing
// decoder, archive and aligned-input cache take it without a second code
// path. Exchange UNSPECIFIED and instrument 0 say "global": the block is the
// same observation for every target, and that is how the trainer pins it.
//
// Status on the wire: VALID whenever any position is usable. This block mixes
// series that trade around the clock with ones that close for the weekend and
// ones published once a day, so a partially filled vector is its normal state
// and the validity bitmap -- position by position -- is the contract. A
// WARMING vector (nothing usable) is still published as WARMING and is
// archived by nobody, the same way the FDW backfill leaves it out.
Result<void> publish(messaging::NatsClient& nats, const MacroVector& v, int64_t computed_at_ms) {
    ::alttech::v1::Envelope env;
    env.set_schema_version(messaging::kSchemaVersion);
    env.set_event_type("feature.macro");
    env.set_exchange(::alttech::v1::EXCHANGE_UNSPECIFIED);
    env.set_instrument_id(0);
    env.set_canonical_pair_id(0);
    env.set_exchange_symbol("GLOBAL");
    env.set_produced_at_ms(computed_at_ms);
    env.set_source_service("alttech-service");
    env.set_message_id("macro:" + std::to_string(v.t_ms));

    auto* fv = env.mutable_feature_vector();
    fv->set_timeframe(::alttech::v1::TF_1M);
    fv->set_bar_open_time_ms(v.t_ms);
    fv->set_bar_revision(0);
    fv->set_feature_set_name(kMacroFeatureSetName);
    fv->set_feature_set_version(kMacroFeatureSetVersion);
    fv->set_schema_hash(macro_schema_fingerprint());
    fv->set_engine_version(kMacroMethodVersion);
    for (const float x : v.values) fv->add_values(x);
    fv->set_validity_bitmap(std::string(v.validity.begin(), v.validity.end()));
    fv->set_status(v.status == VectorStatus::warming ? ::alttech::v1::FEATURE_STATUS_WARMING
                                                     : ::alttech::v1::FEATURE_STATUS_VALID);
    fv->set_computed_at_ms(computed_at_ms);
    // Deterministic id: a minute re-emitted by a later cycle is deduplicated
    // by the stream, so the consumer's first-seen archive and this feed agree.
    return nats.js_publish(messaging::topology::macro_feature_subject(Timeframe::m1),
                           messaging::serialize(env), env.message_id(), 2000);
}

} // namespace

Result<void> run_macro(const YAML::Node& config, persistence::PgPool& pool,
                       messaging::NatsClient& nats,
                       application::ModuleSupervisor& supervisor) {
    (void)nats;   // published through the archive, not a subject; see below
    auto& shutdown = ShutdownToken::instance();

    Params params;
    if (config && config["history_start"]) {
        params.history_start_ms = iso_to_ms(config["history_start"].as<std::string>());
    }
    if (config && config["cycle_interval_sec"]) {
        params.cycle_interval_sec = config["cycle_interval_sec"].as<int>();
    }
    if (config && config["live_lookback_minutes"]) {
        params.live_lookback_minutes = config["live_lookback_minutes"].as<int>();
    }
    if (params.history_start_ms == 0) {
        return std::unexpected(Error{ErrorCode::config_error,
                                     "macro.history_start is required: it tracks AltMacro's "
                                     "coverage, which moves, and a built-in date would be right "
                                     "today and silently wrong later", false});
    }

    auto set_id = ensure_feature_set(pool);
    if (!set_id) {
        log::error("macro_feature_set_failed", {{"error", set_id.error().message}});
        return std::unexpected(set_id.error());
    }
    log::info("macro_feature_set_ready",
              {{"set_id", *set_id}, {"features", static_cast<int64_t>(macro_feature_count())},
               {"schema_hash", macro_schema_fingerprint().substr(0, 12)}});

    supervisor.mark_ready(kModuleName);

    // --- historical backfill, one month at a time --------------------------
    //
    // Chunked so that neither the raw inputs nor the produced block is ever
    // held whole: 284 days of four instruments on both sides is millions of
    // rows. Each chunk reads two extra days in front of itself so the one-day
    // lookbacks and the 1440-minute windows are warm at its first emitted
    // minute.
    int64_t backfilled = 0;
    const int64_t backfill_end = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    // The table retains only partitions within its configured storage window.
    // Start at the oldest available partition, one day inside it, so every
    // backfill chunk has a destination and retention preserves the first chunk
    // while the backfill is running.
    const int64_t backfill_start = std::max(params.history_start_ms, partition_floor_ms(pool));
    if (backfill_start != params.history_start_ms) {
        log::info("macro_backfill_clamped",
                  {{"history_start_ms", params.history_start_ms}, {"from_ms", backfill_start}});
    }
    for (int64_t chunk = backfill_start; chunk < backfill_end; chunk += 30 * kDay) {
        if (shutdown.stop_requested()) break;
        supervisor.heartbeat(kModuleName, params.liveness_budget_ms);

        // Clamped: the last chunk would otherwise run a month past now and try
        // to write minutes that have not happened, into partitions that do not
        // exist because nothing should be writing there.
        const int64_t chunk_end = std::min(chunk + 30 * kDay, backfill_end);
        const auto inputs = load_inputs(pool, chunk - kWarmupMs, chunk_end);
        if (inputs.usdcnh.empty() && inputs.eurusd.empty()) continue;

        auto vectors = compute_macro_series(inputs, chunk - kWarmupMs, chunk_end);
        // Drop the warm-up: those minutes exist only to prime the windows.
        vectors.erase(std::remove_if(vectors.begin(), vectors.end(),
                                     [&](const MacroVector& v) { return v.t_ms < chunk; }),
                      vectors.end());
        auto n = store(pool, *set_id, vectors);
        if (!n) {
            log::error("macro_backfill_store_failed", {{"error", n.error().message}});
            break;
        }
        backfilled += *n;
        log::info("macro_backfill_chunk",
                  {{"from_ms", chunk}, {"rows", *n}, {"total", backfilled}});
    }
    log::info("macro_backfill_done", {{"rows", backfilled}});

    // --- live ---------------------------------------------------------------
    //
    // The trailing window is recomputed and stored every cycle (a daily figure
    // arriving late changes minutes already written), but each minute is
    // PUBLISHED once, when it first exists: the consumer's archive is
    // first-seen, and a vector for minute T is knowable at T -- everything in
    // it is as-of T by construction -- so it goes out in the cycle that first
    // covers T. Nothing before this process started is published; that history
    // reaches the consumer through its own backfill.
    int64_t published_through_ms = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()) / kMinute * kMinute - kMinute;
    while (!shutdown.stop_requested()) {
        supervisor.heartbeat(kModuleName, params.liveness_budget_ms);

        const int64_t now = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        const int64_t from = now - static_cast<int64_t>(params.live_lookback_minutes) * kMinute;

        const auto inputs = load_inputs(pool, from - kWarmupMs, now);
        auto vectors = compute_macro_series(inputs, from - kWarmupMs, now);
        vectors.erase(std::remove_if(vectors.begin(), vectors.end(),
                                     [&](const MacroVector& v) { return v.t_ms < from; }),
                      vectors.end());

        auto n = store(pool, *set_id, vectors);
        if (n) {
            if (*n > 0) {
                log::info("macro_cycle_done",
                          {{"rows", *n}, {"window_minutes", params.live_lookback_minutes}});
            }
        } else {
            log::error("macro_store_failed", {{"error", n.error().message}});
        }

        int64_t sent = 0;
        // Wire availability follows computation and durable storage, rather
        // than the earlier cycle-start timestamp used to select observations.
        const int64_t available_at_ms = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        for (const auto& v : vectors) {
            if (!n) break; // retry next cycle; never publish an unarchived block
            if (v.t_ms <= published_through_ms || v.t_ms > now) continue;
            if (auto p = publish(nats, v, available_at_ms); !p) {
                log::warn("macro_publish_failed",
                          {{"bar_open_ms", v.t_ms}, {"error", p.error().message}});
                break;   // keep order: retry from this minute next cycle
            }
            published_through_ms = v.t_ms;
            ++sent;
        }
        if (sent > 0) {
            log::info("macro_published", {{"vectors", sent}, {"through_ms", published_through_ms}});
        }

        if (shutdown.wait_for(std::chrono::seconds(params.cycle_interval_sec))) break;
    }
    return {};
}

} // namespace alttech::modules
