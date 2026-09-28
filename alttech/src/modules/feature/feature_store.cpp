#include "feature_store.hpp"
#include "service_logic.hpp"

#include "alttech/persistence/sql_literal.hpp"
#include "alttech/domain/types.hpp"
#include "alttech/logging/logger.hpp"
#include "alttech/persistence/txn_log.hpp"
#include "alttech/time/time.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace alttech::featsvc {

using namespace alttech::talib_features;

namespace {
using persistence::sql::append_bytea;


// The venue is a parameter now, not a constant. Every store call is scoped to
// one venue and derives both the exchange id and the partition class from it.
// These used to be fixed at Bitget, which is what confined the whole TA-Lib
// engine to a single venue.
constexpr const char* kProfile = "talib_core";
constexpr const char* kCheckpointModule = "feature";

void append_float_array(std::string& sql, const std::vector<float>& v) {
    sql += "'{";
    char buf[32];
    for (size_t i = 0; i < v.size(); ++i) {
        if (i != 0) sql += ',';
        std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v[i]));
        sql += buf;
    }
    sql += "}'::real[]";
}


} // namespace

Result<int64_t> ensure_core_feature_set(persistence::PgPool& pool) {
    const auto& names = core_feature_names();
    const std::string hash = core_schema_fingerprint();
    int64_t set_id = 0;
    std::string existing_hash;
    std::string existing_engine;
    bool existing_frozen = false;
    bool found_existing = false;
    bool positions_contiguous = true;
    std::vector<std::string> existing_names;

    auto res = pool.with_txn([&](pqxx::work& txn) {
        const auto rows = txn.exec_params(
            "SELECT id, schema_hash, engine_version, frozen FROM alttech.feature_set_versions "
            "WHERE name = $1 AND version = $2",
            std::string(kCoreSetName), kCoreSetVersion);
        if (!rows.empty()) {
            found_existing = true;
            set_id = rows[0][0].as<int64_t>();
            existing_hash = rows[0][1].as<std::string>();
            existing_engine = rows[0][2].as<std::string>();
            existing_frozen = rows[0][3].as<bool>();
            const auto members = txn.exec_params(
                "SELECT position, output_name FROM alttech.feature_set_members "
                "WHERE feature_set_version_id = $1 ORDER BY position",
                set_id);
            existing_names.reserve(members.size());
            for (int i = 0; i < members.size(); ++i) {
                if (members[i][0].as<int>() != i) positions_contiguous = false;
                existing_names.push_back(members[i][1].as<std::string>());
            }
            return;
        }
        const auto ins = txn.exec_params(
            "INSERT INTO alttech.feature_set_versions "
            "(name, version, scope, profile, engine_version, schema_hash, frozen) "
            "VALUES ($1, $2, 'VENUE', $3, $4, $5, true) RETURNING id",
            std::string(kCoreSetName), kCoreSetVersion, std::string(kProfile),
            std::string(kEngineVersion), hash);
        set_id = ins[0][0].as<int64_t>();
        for (size_t i = 0; i < names.size(); ++i) {
            const auto def = txn.exec_params(
                "INSERT INTO alttech.feature_definitions (name, family, scope) "
                "VALUES ($1, 'talib_core', 'VENUE') "
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
    if (found_existing &&
        (existing_hash != hash || existing_engine != kEngineVersion || !existing_frozen ||
         !positions_contiguous || existing_names != names)) {
        return fail(ErrorCode::config_error,
                    "frozen feature metadata mismatch for " + std::string(kCoreSetName) +
                        " v" + std::to_string(kCoreSetVersion) + " (db hash " +
                        existing_hash.substr(0, 12) + " engine " + existing_engine +
                        ", code hash " + hash.substr(0, 12) + " engine " + kEngineVersion + ")");
    }
    return set_id;
}

Result<std::vector<ShardCandle>> load_candles(persistence::PgPool& pool, Venue venue, int64_t instrument_id,
                                              const std::string& tf_label, int64_t from_ms,
                                              int64_t to_ms, int limit) {
    std::vector<ShardCandle> out;
    if (instrument_id <= 0) {
        return fail(ErrorCode::validation_error, "instrument_id must be positive");
    }
    if (!tf_from_label(tf_label)) {
        return fail(ErrorCode::validation_error, "unsupported candle timeframe: " + tf_label);
    }
    if (from_ms < 0 || limit <= 0 || limit > 100000 ||
        (to_ms != std::numeric_limits<int64_t>::max() && to_ms <= from_ms)) {
        return fail(ErrorCode::validation_error, "invalid candle replay range or limit");
    }
    const bool open_ended = to_ms == std::numeric_limits<int64_t>::max();
    const auto plan = candle_query_plan(venue, tf_label, open_ended, limit);
    auto res = pool.with_txn([&](pqxx::work& txn) {
        txn.exec("SET LOCAL TIME ZONE 'UTC'");
        pqxx::result rows;
        if (plan.one_minute) {
            rows = open_ended ? txn.exec_params(plan.sql, instrument_id, from_ms)
                              : txn.exec_params(plan.sql, instrument_id, from_ms, to_ms);
        } else {
            rows = open_ended ? txn.exec_params(plan.sql, instrument_id, from_ms, tf_label)
                              : txn.exec_params(plan.sql, instrument_id, from_ms, to_ms, tf_label);
        }
        out.reserve(rows.size());
        for (const auto& r : rows) {
            ShardCandle c;
            c.bar.open_time_ms = r[0].as<int64_t>();
            c.bar.open = r[1].as<double>();
            c.bar.high = r[2].as<double>();
            c.bar.low = r[3].as<double>();
            c.bar.close = r[4].as<double>();
            c.bar.base_volume = r[5].as<double>();
            c.bar.quote_volume = r[6].as<double>();
            c.revision = r[7].as<int64_t>();
            c.quality_ok = r[8].as<bool>();
            out.push_back(c);
        }
    });
    if (!res) return std::unexpected(res.error());
    return out;
}

Result<std::vector<ShardCandle>> load_candles_at(persistence::PgPool& pool, Venue venue,
                                                int64_t instrument_id,
                                                const std::string& tf_label,
                                                const std::vector<int64_t>& open_times_ms) {
    std::vector<ShardCandle> out;
    if (instrument_id <= 0) {
        return fail(ErrorCode::validation_error, "instrument_id must be positive");
    }
    if (!tf_from_label(tf_label)) {
        return fail(ErrorCode::validation_error, "unsupported candle timeframe: " + tf_label);
    }
    if (open_times_ms.empty()) return out;

    // A bigint[] literal, so the query asks for exactly these bars and can
    // never return a truncated prefix of the window containing them.
    std::string array_literal = "{";
    for (size_t i = 0; i < open_times_ms.size(); ++i) {
        if (open_times_ms[i] < 0) {
            return fail(ErrorCode::validation_error, "negative candle open time");
        }
        if (i != 0) array_literal += ',';
        array_literal += std::to_string(open_times_ms[i]);
    }
    array_literal += '}';

    const std::string sql = candles_at_query(venue, tf_label);
    auto res = pool.with_txn([&](pqxx::work& txn) {
        txn.exec("SET LOCAL TIME ZONE 'UTC'");
        const pqxx::result rows =
            tf_label == "1m"
                ? txn.exec_params(sql, instrument_id, array_literal)
                : txn.exec_params(sql, instrument_id, array_literal, tf_label);
        out.reserve(rows.size());
        for (const auto& r : rows) {
            ShardCandle c;
            c.bar.open_time_ms = r[0].as<int64_t>();
            c.bar.open = r[1].as<double>();
            c.bar.high = r[2].as<double>();
            c.bar.low = r[3].as<double>();
            c.bar.close = r[4].as<double>();
            c.bar.base_volume = r[5].as<double>();
            c.bar.quote_volume = r[6].as<double>();
            c.revision = r[7].as<int64_t>();
            c.quality_ok = r[8].as<bool>();
            out.push_back(c);
        }
    });
    if (!res) return std::unexpected(res.error());
    return out;
}

Result<std::optional<ShardCandle>> load_persisted_candle(persistence::PgPool& pool, Venue venue,
                                                        int64_t instrument_id,
                                                        const std::string& tf_label,
                                                        int64_t open_time_ms,
                                                        int64_t span_ms) {
    if (span_ms <= 0 || open_time_ms > std::numeric_limits<int64_t>::max() - span_ms) {
        return fail(ErrorCode::validation_error, "invalid exact candle range");
    }
    auto rows = load_candles(pool, venue, instrument_id, tf_label, open_time_ms,
                             open_time_ms + span_ms, 2);
    if (!rows) return std::unexpected(rows.error());
    if (rows->empty() || rows->front().bar.open_time_ms != open_time_ms) {
        return std::optional<ShardCandle>{};
    }
    return std::optional<ShardCandle>{std::move(rows->front())};
}

Result<std::optional<int64_t>> last_vector_time(persistence::PgPool& pool, Venue venue, int64_t instrument_id,
                                                const std::string& tf_label,
                                                int64_t feature_set_id) {
    std::optional<int64_t> out;
    auto res = pool.with_txn([&](pqxx::work& txn) {
        const auto r = txn.exec_params(
            "SELECT (extract(epoch FROM max(bar_open_time)) * 1000)::bigint "
            "FROM alttech.venue_feature_vectors WHERE venue_class = $5 "
            "AND exchange_id = $6 AND canonical_pair_id = $1 AND timeframe = $2 "
            "AND feature_set_version_id = $3 AND engine_version = $4",
            instrument_id, tf_label, feature_set_id, std::string(kEngineVersion),
            std::string(venue_feature_class(venue)), venue_id(venue));
        if (!r.empty() && !r[0][0].is_null()) out = r[0][0].as<int64_t>();
    });
    if (!res) return std::unexpected(res.error());
    return out;
}

Result<std::optional<int64_t>> first_missing_vector_time(
    persistence::PgPool& pool, Venue venue, int64_t instrument_id,
    const std::string& tf_label,
    int64_t feature_set_id, int64_t from_ms) {
    if (instrument_id <= 0 || !tf_from_label(tf_label) || feature_set_id <= 0 || from_ms < 0) {
        return fail(ErrorCode::validation_error, "invalid missing-vector query arguments");
    }
    const auto plan = missing_vector_query_plan(venue, tf_label);
    std::optional<int64_t> out;
    auto result = pool.with_txn([&](pqxx::work& txn) {
        const auto rows = txn.exec_params(plan.sql, instrument_id, tf_label, feature_set_id,
                                          std::string(kEngineVersion), from_ms);
        if (!rows.empty()) out = rows[0][0].as<int64_t>();
    });
    if (!result) return std::unexpected(result.error());
    return out;
}

Result<std::optional<int64_t>> first_stale_warming_vector_time(
    persistence::PgPool& pool, Venue venue, int64_t instrument_id, const std::string& tf_label,
    int64_t feature_set_id, int64_t from_ms, int64_t min_context_bars) {
    if (instrument_id <= 0 || !tf_from_label(tf_label) || feature_set_id <= 0 || from_ms < 0 ||
        min_context_bars <= 0) {
        return fail(ErrorCode::validation_error, "invalid stale-vector query arguments");
    }
    const bool is_1m = tf_label == "1m";
    std::optional<int64_t> out;
    auto result = pool.with_txn([&](pqxx::work& txn) {
        txn.exec("SET LOCAL TIME ZONE 'UTC'");
        // The oldest WARMING vector that has at least `min_context_bars` closed
        // source bars before it. Counting the bars is what separates "the
        // history has since arrived" from "this bar genuinely follows a gap".
        std::string sql =
            "WITH warming AS ("
            "  SELECT v.bar_open_time"
            "  FROM alttech.venue_feature_vectors v"
            "  WHERE v.venue_class = '" + std::string(venue_feature_class(venue)) + "'"
            "    AND v.exchange_id = " + std::to_string(venue_id(venue)) +
            "    AND v.canonical_pair_id = $1 AND v.timeframe = $2"
            "    AND v.feature_set_version_id = $3 AND v.engine_version = $4"
            "    AND v.status = 'WARMING'"
            "    AND v.bar_open_time >= to_timestamp($5::bigint / 1000.0)"
            "  ORDER BY v.bar_open_time LIMIT 1)"
            " SELECT (extract(epoch FROM w.bar_open_time) * 1000)::bigint"
            " FROM warming w"
            " WHERE (SELECT count(*) FROM ";
        sql += is_1m ? "market_candles_1m c" : "market_candles_agg c";
        sql += " WHERE c.exchange_id = " + std::to_string(venue_id(venue)) +
               "   AND c.instrument_id = $1 AND c.is_closed"
               "   AND c.open_time < w.bar_open_time";
        if (!is_1m) sql += " AND c.timeframe = $2";
        sql += ") >= $6";

        const auto rows = txn.exec_params(sql, instrument_id, tf_label, feature_set_id,
                                          std::string(kEngineVersion), from_ms, min_context_bars);
        if (!rows.empty() && !rows[0][0].is_null()) out = rows[0][0].as<int64_t>();
    });
    if (!result) return std::unexpected(result.error());
    return out;
}

Result<bool> current_vector_exists(persistence::PgPool& pool, Venue venue, int64_t instrument_id,
                                   const std::string& tf_label, int64_t open_time_ms,
                                   int64_t feature_set_id) {
    bool exists = false;
    const bool is_1m = tf_label == "1m";
    if (!tf_from_label(tf_label)) {
        return fail(ErrorCode::validation_error, "unsupported candle timeframe: " + tf_label);
    }
    auto res = pool.with_txn([&](pqxx::work& txn) {
        std::string sql = "SELECT EXISTS (SELECT 1 FROM ";
        sql += is_1m ? "market_candles_1m c" : "market_candles_agg c";
        sql +=
            " JOIN alttech.venue_feature_vectors v ON v.venue_class = '" + std::string(venue_feature_class(venue)) + "' "
            "AND v.exchange_id = c.exchange_id AND v.canonical_pair_id = c.instrument_id "
            "AND v.timeframe = $2 AND v.bar_open_time = c.open_time "
            "AND v.bar_revision = c.revision AND v.feature_set_version_id = $4 "
            "AND v.engine_version = $5 "
            "WHERE c.exchange_id = " + std::to_string(venue_id(venue)) + " AND c.instrument_id = $1 "
            "AND c.open_time = to_timestamp($3::bigint / 1000.0) AND c.is_closed";
        if (!is_1m) sql += " AND c.timeframe = $2";
        sql += ")";
        exists = txn.exec_params1(sql, instrument_id, tf_label, open_time_ms, feature_set_id,
                                  std::string(kEngineVersion))[0]
                     .as<bool>();
    });
    if (!res) return std::unexpected(res.error());
    return exists;
}

Result<std::optional<VectorRow>> load_current_vector(
    persistence::PgPool& pool, Venue venue, int64_t instrument_id,
    const std::string& tf_label,
    int64_t open_time_ms, int64_t feature_set_id, int64_t bar_revision) {
    if (instrument_id <= 0 || !tf_from_label(tf_label) || open_time_ms < 0 ||
        feature_set_id <= 0 || bar_revision < 0) {
        return fail(ErrorCode::validation_error, "invalid exact-vector query arguments");
    }

    std::optional<VectorRow> out;
    auto res = pool.with_txn([&](pqxx::work& txn) {
        const auto rows = txn.exec_params(
            "SELECT v.values, v.validity_bitmap, v.status, v.engine_version, "
            "       (extract(epoch FROM v.computed_at) * 1000)::bigint "
            "FROM alttech.venue_feature_vectors v WHERE v.venue_class = '" + std::string(venue_feature_class(venue)) + "' "
            "AND v.exchange_id = " + std::to_string(venue_id(venue)) + " AND v.canonical_pair_id = $1 AND v.timeframe = $2 "
            "AND v.bar_open_time = to_timestamp($3::bigint / 1000.0) "
            "AND v.feature_set_version_id = $4 AND v.bar_revision = $5 "
            "AND v.engine_version = $6 "
            "AND EXISTS (SELECT 1 FROM "
            "  (SELECT revision FROM market_candles_1m WHERE $2 = '1m' "
            "   AND exchange_id = " + std::to_string(venue_id(venue)) + " AND instrument_id = $1 "
            "   AND open_time = to_timestamp($3::bigint / 1000.0) "
            "   UNION ALL "
            "   SELECT revision FROM market_candles_agg WHERE $2 <> '1m' "
            "   AND exchange_id = " + std::to_string(venue_id(venue)) + " AND instrument_id = $1 AND timeframe = $2 "
            "   AND open_time = to_timestamp($3::bigint / 1000.0)) c "
            "  WHERE c.revision = v.bar_revision) LIMIT 1",
            instrument_id, tf_label, open_time_ms, feature_set_id, bar_revision,
            std::string(kEngineVersion));
        if (rows.empty()) return;

        VectorRow row;
        row.instrument_id = instrument_id;
        row.timeframe = tf_label;
        row.vec.bar_open_time_ms = open_time_ms;
        row.bar_revision = bar_revision;
        row.quality = rows[0][2].as<std::string>();
        row.engine_version = rows[0][3].as<std::string>();
        row.computed_at_ms = rows[0][4].as<int64_t>();
        if (row.quality == "VALID") row.vec.status = VectorStatus::valid;
        else if (row.quality == "WARMING") row.vec.status = VectorStatus::warming;
        else if (row.quality == "NOT_READY") row.vec.status = VectorStatus::not_ready;
        else if (row.quality == "INCOMPLETE_INPUT") row.vec.status = VectorStatus::incomplete_input;
        else if (row.quality == "REVISED") row.vec.status = VectorStatus::revised;
        else if (row.quality == "INVALID") row.vec.status = VectorStatus::invalid;
        else throw std::runtime_error("unknown durable feature vector status");

        // The column is NOT NULL and every element is written from a finite
        // float, so a NULL element would mean the row was written by something
        // other than this engine; as_sql_array<float> rejects it for us.
        const auto values = rows[0][0].as_sql_array<float>();
        row.vec.values.assign(values.cbegin(), values.cend());
        const auto validity = rows[0][1].as<std::basic_string<std::byte>>();
        row.vec.validity.reserve(validity.size());
        for (const auto byte : validity) {
            row.vec.validity.push_back(std::to_integer<uint8_t>(byte));
        }
        out = std::move(row);
    });
    if (!res) return std::unexpected(res.error());
    if (out && (out->vec.values.size() != FeatureEngine::feature_count() ||
                out->vec.validity.size() != (FeatureEngine::feature_count() + 7U) / 8U ||
                !std::all_of(out->vec.values.begin(), out->vec.values.end(),
                             [](float value) { return std::isfinite(value); }) ||
                out->engine_version != kEngineVersion || out->computed_at_ms <= 0)) {
        return fail(ErrorCode::validation_error,
                    "durable feature vector has invalid width, bitmap, engine, or timestamp");
    }
    return out;
}

Result<bool> current_vectors_complete(persistence::PgPool& pool, Venue venue, int64_t instrument_id,
                                      const std::string& tf_label, int64_t from_ms,
                                      int64_t to_ms, int64_t feature_set_id) {
    bool complete = false;
    const bool is_1m = tf_label == "1m";
    if (!tf_from_label(tf_label) || from_ms < 0 || to_ms <= from_ms) {
        return fail(ErrorCode::validation_error, "invalid feature completion range");
    }
    auto res = pool.with_txn([&](pqxx::work& txn) {
        std::string sql = "SELECT NOT EXISTS (SELECT 1 FROM ";
        sql += is_1m ? "market_candles_1m c" : "market_candles_agg c";
        sql +=
            " WHERE c.exchange_id = " + std::to_string(venue_id(venue)) + " AND c.instrument_id = $1 AND c.is_closed "
            "AND c.open_time >= to_timestamp($3::bigint / 1000.0) "
            "AND c.open_time < to_timestamp($4::bigint / 1000.0)";
        if (!is_1m) sql += " AND c.timeframe = $2";
        sql +=
            " AND NOT EXISTS (SELECT 1 FROM alttech.venue_feature_vectors v "
            "WHERE v.venue_class = '" + std::string(venue_feature_class(venue)) + "' AND v.exchange_id = c.exchange_id "
            "AND v.canonical_pair_id = c.instrument_id AND v.timeframe = $2 "
            "AND v.bar_open_time = c.open_time AND v.bar_revision = c.revision "
            "AND v.feature_set_version_id = $5 AND v.engine_version = $6))";
        complete = txn.exec_params1(sql, instrument_id, tf_label, from_ms, to_ms, feature_set_id,
                                    std::string(kEngineVersion))[0]
                       .as<bool>();
    });
    if (!res) return std::unexpected(res.error());
    return complete;
}

Result<std::optional<int64_t>> first_incomplete_bar(persistence::PgPool& pool, Venue venue,
                                                    int64_t instrument_id,
                                                    const std::string& tf_label, int64_t from_ms,
                                                    int64_t to_ms, int64_t feature_set_id) {
    std::optional<int64_t> earliest;
    const bool is_1m = tf_label == "1m";
    if (!tf_from_label(tf_label) || from_ms < 0 || to_ms <= from_ms) {
        return fail(ErrorCode::validation_error, "invalid feature completion range");
    }
    auto res = pool.with_txn([&](pqxx::work& txn) {
        // The same predicate current_vectors_complete uses, but returning WHICH
        // bar is missing rather than merely whether one is. Kept adjacent to it
        // on purpose: two predicates that must agree should be read together.
        std::string sql = "SELECT (extract(epoch FROM min(c.open_time)) * 1000)::bigint FROM ";
        sql += is_1m ? "market_candles_1m c" : "market_candles_agg c";
        sql +=
            " WHERE c.exchange_id = " + std::to_string(venue_id(venue)) + " AND c.instrument_id = $1 AND c.is_closed "
            "AND c.open_time >= to_timestamp($3::bigint / 1000.0) "
            "AND c.open_time < to_timestamp($4::bigint / 1000.0)";
        if (!is_1m) sql += " AND c.timeframe = $2";
        sql +=
            " AND NOT EXISTS (SELECT 1 FROM alttech.venue_feature_vectors v "
            "WHERE v.venue_class = '" + std::string(venue_feature_class(venue)) + "' AND v.exchange_id = c.exchange_id "
            "AND v.canonical_pair_id = c.instrument_id AND v.timeframe = $2 "
            "AND v.bar_open_time = c.open_time AND v.bar_revision = c.revision "
            "AND v.feature_set_version_id = $5 AND v.engine_version = $6)";
        const auto row = txn.exec_params1(sql, instrument_id, tf_label, from_ms, to_ms,
                                          feature_set_id, std::string(kEngineVersion));
        if (!row[0].is_null()) earliest = row[0].as<int64_t>();
    });
    if (!res) return std::unexpected(res.error());
    return earliest;
}

Result<int64_t> write_vectors(persistence::PgPool& pool, Venue venue, int64_t feature_set_id,
                              const std::vector<VectorRow>& rows,
                              const std::map<std::string, size_t>& name_index) {
    if (rows.empty()) return 0;
    const size_t expected_values = FeatureEngine::feature_count();
    const size_t expected_bitmap = (expected_values + 7U) / 8U;
    for (const auto& row : rows) {
        if (row.instrument_id <= 0 || !tf_from_label(row.timeframe) || row.bar_revision < 0 ||
            row.vec.bar_open_time_ms < 0 || row.vec.values.size() != expected_values ||
            row.vec.validity.size() != expected_bitmap ||
            !std::all_of(row.vec.values.begin(), row.vec.values.end(),
                         [](float value) { return std::isfinite(value); }) ||
            row.engine_version != kEngineVersion || row.computed_at_ms <= 0) {
            return fail(ErrorCode::validation_error, "invalid feature vector write metadata");
        }
    }

    // The flat projection is mapped onto the TA-Lib catalog by NAME, never by
    // position. A column with no TA-Lib counterpart is written NULL rather than
    // filled with a lookalike. bb_width_20 and volume_zscore_20 are ratios of
    // TA-Lib outputs, not new indicator math.
    static const char* kProjection[] = {
        "EMA_20_outReal", "EMA_50_outReal", "EMA_200_outReal", "RSI_14_outReal",
        "ATR_14_outReal", "ADX_14_outReal",
        "",                  // bb_width_20      -> derived below
        "",                  // volume_zscore_20 -> derived below
        "STDDEV_20_outReal", // realized_vol_20
        ""};                 // efficiency_ratio_20 -> no TA-Lib equivalent
    static constexpr size_t kBbWidthColumn = 6;
    static constexpr size_t kVolumeZScoreColumn = 7;
    static const char* kBbUpper = "BBANDS_20_SMA_outRealUpperBand";
    static const char* kBbLower = "BBANDS_20_SMA_outRealLowerBand";
    static const char* kBbMiddle = "BBANDS_20_SMA_outRealMiddleBand";
    static const char* kVolumeSma = "SMA_VOL_20_outReal";
    static const char* kVolumeStddev = "STDDEV_VOL_20_outReal";

    auto res = pool.with_txn([&](pqxx::work& txn) {
        txn.exec("SET LOCAL TIME ZONE 'UTC'");
        std::string sql =
            "INSERT INTO alttech.venue_feature_vectors (venue_class, exchange_id, "
            "canonical_pair_id, timeframe, bar_open_time, bar_revision, feature_set_version_id, "
            "profile, values, validity_bitmap, status, engine_version, computed_at) VALUES ";
        bool first = true;
        for (const auto& r : rows) {
            sql += first ? "(" : ",(";
            first = false;
            sql += txn.quote(std::string(venue_feature_class(venue))) + "," + std::to_string(venue_id(venue)) +
                   "," + std::to_string(r.instrument_id) + "," + txn.quote(r.timeframe) +
                   ",to_timestamp(" + std::to_string(r.vec.bar_open_time_ms) +
                   "::bigint / 1000.0)," + std::to_string(r.bar_revision) + "," +
                   std::to_string(feature_set_id) + "," + txn.quote(std::string(kProfile)) + ",";
            append_float_array(sql, r.vec.values);
            sql += ",";
            append_bytea(sql, r.vec.validity);
            sql += "," + txn.quote(r.quality) + "," + txn.quote(r.engine_version) +
                   ",to_timestamp(" + std::to_string(r.computed_at_ms) + "::bigint / 1000.0))";
        }
        sql += " ON CONFLICT (venue_class, exchange_id, canonical_pair_id, timeframe, "
               "bar_open_time, bar_revision, feature_set_version_id) DO UPDATE SET "
               "values = EXCLUDED.values, validity_bitmap = EXCLUDED.validity_bitmap, "
               "status = EXCLUDED.status, engine_version = EXCLUDED.engine_version, "
               "computed_at = EXCLUDED.computed_at, updated_at = now()";
        txn.exec(sql);

        // The projection keeps days where the vectors keep weeks (retention 4 d
        // against 14 d). A replayed bar older than the projection's oldest
        // partition has nowhere to put its projection row, and the INSERT used
        // to fail the whole write -- the vector with it -- until an hourly
        // partition advance happened to recreate the day that retention then
        // dropped again. Such a row is exactly what retention would reclaim, so
        // it is not written. Live bars never take this path: the floor is only
        // read when the batch reaches back more than a day.
        int64_t projection_floor_ms = 0;
        {
            int64_t oldest_ms = std::numeric_limits<int64_t>::max();
            for (const auto& r : rows) oldest_ms = std::min(oldest_ms, r.vec.bar_open_time_ms);
            if (oldest_ms < timeutil::now_ms() - 86'400'000) {
                const auto floor = txn.exec(
                    "SELECT (extract(epoch FROM min(lower_bound)) * 1000)::bigint "
                    "FROM alttech.partition_bounds('alttech.venue_feature_projection'::regclass)");
                projection_floor_ms = floor[0][0].is_null() ? std::numeric_limits<int64_t>::max()
                                                            : floor[0][0].as<int64_t>();
            }
        }

        std::string projection =
            "INSERT INTO alttech.venue_feature_projection (exchange_id, canonical_pair_id, "
            "timeframe, bar_open_time, bar_revision, ema_20, ema_50, ema_200, rsi_14, atr_14, "
            "adx_14, bb_width_20, volume_zscore_20, realized_vol_20, efficiency_ratio_20) VALUES ";
        first = true;
        for (const auto& r : rows) {
            if (r.vec.bar_open_time_ms < projection_floor_ms) continue;
            projection += first ? "(" : ",(";
            first = false;
            projection += std::to_string(venue_id(venue)) + "," +
                          std::to_string(r.instrument_id) + "," + txn.quote(r.timeframe) +
                          ",to_timestamp(" + std::to_string(r.vec.bar_open_time_ms) +
                          "::bigint / 1000.0)," + std::to_string(r.bar_revision);
            // Safe name lookup: an absent or invalid feature yields NULL.
            const auto value_of = [&](const char* name) -> std::optional<double> {
                if (name == nullptr || *name == '\0') return std::nullopt;
                const auto it = name_index.find(name);
                if (it == name_index.end() || !r.vec.is_valid(it->second)) return std::nullopt;
                return static_cast<double>(r.vec.values[it->second]);
            };
            const auto append = [&](std::optional<double> v) {
                if (!v || !std::isfinite(*v)) {
                    projection += ",NULL";
                    return;
                }
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.9g", *v);
                projection += std::string(",") + buf;
            };
            for (size_t column = 0; column < std::size(kProjection); ++column) {
                if (column == kVolumeZScoreColumn) {
                    const auto sma = value_of(kVolumeSma);
                    const auto sd = value_of(kVolumeStddev);
                    append(sma && sd && *sd > 0.0
                               ? std::optional<double>((r.bar_volume - *sma) / *sd)
                               : std::nullopt);
                } else if (column == kBbWidthColumn) {
                    const auto upper = value_of(kBbUpper);
                    const auto lower = value_of(kBbLower);
                    const auto middle = value_of(kBbMiddle);
                    append(upper && lower && middle && *middle != 0.0
                               ? std::optional<double>((*upper - *lower) / *middle)
                               : std::nullopt);
                } else {
                    append(value_of(kProjection[column]));
                }
            }
            projection += ")";
        }
        projection +=
            " ON CONFLICT (exchange_id, canonical_pair_id, timeframe, bar_open_time) DO UPDATE SET "
            "bar_revision = EXCLUDED.bar_revision, ema_20 = EXCLUDED.ema_20, "
            "ema_50 = EXCLUDED.ema_50, ema_200 = EXCLUDED.ema_200, rsi_14 = EXCLUDED.rsi_14, "
            "atr_14 = EXCLUDED.atr_14, adx_14 = EXCLUDED.adx_14, "
            "bb_width_20 = EXCLUDED.bb_width_20, volume_zscore_20 = EXCLUDED.volume_zscore_20, "
            "realized_vol_20 = EXCLUDED.realized_vol_20, "
            "efficiency_ratio_20 = EXCLUDED.efficiency_ratio_20, updated_at = now()";
        if (!first) txn.exec(projection);
    });
    if (!res) return std::unexpected(res.error());
    return static_cast<int64_t>(rows.size());
}

Result<void> save_checkpoint(persistence::PgPool& pool, Venue venue, int64_t instrument_id,
                             const std::string& tf_label, int64_t last_bar_ms) {
    return pool.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "INSERT INTO alttech.module_checkpoints (module, exchange_id, canonical_pair_id, "
            "timeframe, engine_version, last_event_time, state) "
            "VALUES ($5, 1, $1, $2, $3, to_timestamp($4::bigint / 1000.0), "
            "convert_to('REPLAY_CONTIGUOUS', 'UTF8')) "
            "ON CONFLICT (module, exchange_id, canonical_pair_id, timeframe, engine_version) "
            "DO UPDATE SET last_event_time = EXCLUDED.last_event_time, state = EXCLUDED.state, "
            "updated_at = now()",
            instrument_id, tf_label, std::string(kEngineVersion), last_bar_ms,
            std::string(kCheckpointModule));
    });
}

Result<int64_t> open_run(persistence::PgPool& pool, Venue venue, const char* mode, int64_t instrument_id,
                         const std::string& tf_label) {
    int64_t id = 0;
    auto res = pool.with_txn([&](pqxx::work& txn) {
        const auto r = txn.exec_params(
            "INSERT INTO alttech.analytics_computation_runs "
            "(module, mode, exchange_id, canonical_pair_id, timeframe) "
            "VALUES ('feature', $1, 1, $2, $3) RETURNING id",
            std::string(mode), instrument_id, tf_label);
        id = r[0][0].as<int64_t>();
    });
    if (!res) return std::unexpected(res.error());
    return id;
}

void close_run(persistence::PgPool& pool, int64_t run_id, int64_t vectors, bool ok,
               const std::string& error) {
    persistence::with_txn_logged(pool, "feature_run_close", [&](pqxx::work& txn) {
        txn.exec_params(
            "UPDATE alttech.analytics_computation_runs SET records_written = $2, status = $3, "
            "error = NULLIF($4, ''), finished_at = now() WHERE id = $1",
            run_id, vectors, ok ? "completed" : "failed", error);
    });
}

} // namespace alttech::featsvc
