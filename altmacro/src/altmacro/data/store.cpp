#include "altmacro/data/store.hpp"

#include "altmacro/core/logger.hpp"
#include "altmacro/core/time.hpp"

#include <ctime>
#include <sstream>

namespace altmacro::data {
namespace {

constexpr int64_t kDayMs = 86'400'000;

std::string ts(int64_t ms) { return timeutil::to_iso8601(ms); }

// Month boundaries for partitioning, in UTC.
std::string month_key(int64_t ms) {
    const std::time_t s = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    gmtime_r(&s, &tm);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d%02d", tm.tm_year + 1900, tm.tm_mon + 1);
    return buf;
}

int64_t month_start_ms(int64_t ms) {
    const std::time_t s = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    gmtime_r(&s, &tm);
    tm.tm_mday = 1; tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
    return static_cast<int64_t>(timegm(&tm)) * 1000;
}

int64_t next_month_ms(int64_t ms) {
    const std::time_t s = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    gmtime_r(&s, &tm);
    tm.tm_mday = 1; tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
    if (++tm.tm_mon > 11) { tm.tm_mon = 0; ++tm.tm_year; }
    return static_cast<int64_t>(timegm(&tm)) * 1000;
}

} // namespace

Result<std::vector<SourceBinding>> Store::load_bindings() {
    std::vector<SourceBinding> out;
    auto r = pool_.with_txn([&](pqxx::work& txn) {
        const auto rows = txn.exec(
            "SELECT s.instrument_id, s.provider_id, i.code, p.code, s.native_symbol, "
            "       i.series_class, i.denomination, i.native_frequency, i.session_calendar, "
            "       s.price_basis, s.timestamp_semantics, s.source_timezone, s.priority, "
            "       s.decode_scale, s.sanity_min, s.sanity_max, "
            "       (s.enabled AND i.enabled AND p.enabled) AS usable "
            "FROM macro_instrument_sources s "
            "JOIN macro_instruments i ON i.id = s.instrument_id "
            "JOIN macro_providers   p ON p.id = s.provider_id "
            "ORDER BY i.code, s.price_basis, s.priority");
        for (const auto& row : rows) {
            SourceBinding b;
            b.instrument_id = row[0].as<int64_t>();
            b.provider_id = row[1].as<int16_t>();
            b.instrument_code = row[2].as<std::string>();
            b.provider_code = row[3].as<std::string>();
            b.native_symbol = row[4].as<std::string>();
            b.series_class = row[5].as<std::string>();
            b.denomination = row[6].as<std::string>();
            b.native_frequency = row[7].as<std::string>();
            b.session_calendar = row[8].as<std::string>();
            const auto basis = domain::price_basis_from_string(row[9].as<std::string>());
            b.price_basis = basis.value_or(domain::PriceBasis::not_applicable);
            b.timestamp_semantics = row[10].as<std::string>();
            b.source_timezone = row[11].as<std::string>();
            b.priority = row[12].as<int16_t>();
            if (!row[13].is_null()) b.decode_scale = row[13].as<double>();
            if (!row[14].is_null()) b.sanity_min = row[14].as<double>();
            if (!row[15].is_null()) b.sanity_max = row[15].as<double>();
            b.enabled = row[16].as<bool>();
            out.push_back(std::move(b));
        }
    });
    if (!r) return std::unexpected(r.error());
    return out;
}

Result<std::vector<int64_t>> Store::upsert_bars(const SourceBinding& binding,
                                                const std::string& timeframe,
                                                const std::vector<domain::PriceBar>& bars) {
    std::vector<int64_t> written;
    if (bars.empty()) return written;
    auto r = pool_.with_txn([&](pqxx::work& txn) {
        // A temp table plus one INSERT ... ON CONFLICT is both fast and exactly
        // idempotent. Inserting row by row would be neither.
        txn.exec("CREATE TEMP TABLE _bars (LIKE macro_price_bars INCLUDING DEFAULTS) "
                 "ON COMMIT DROP");
        {
            pqxx::stream_to stream = pqxx::stream_to::table(
                txn, {"_bars"},
                {"instrument_id", "provider_id", "timeframe", "event_time", "ingest_time",
                 "open", "high", "low", "close", "volume", "price_basis",
                 "quality_status", "quality_reason"});
            const std::string now = ts(timeutil::now_ms());
            for (const auto& b : bars) {
                stream << std::make_tuple(
                    binding.instrument_id, binding.provider_id, timeframe,
                    ts(b.event_time_ms), now,
                    b.open, b.high, b.low, b.close, b.volume,
                    std::string(domain::to_string(b.basis)),
                    std::string(domain::to_string(b.quality)),
                    b.quality_reason.empty() ? std::optional<std::string>{}
                                             : std::optional<std::string>{b.quality_reason});
            }
            stream.complete();
        }
        const auto res = txn.exec(
            "INSERT INTO macro_price_bars "
            "  (instrument_id, provider_id, timeframe, event_time, ingest_time, "
            "   open, high, low, close, volume, price_basis, quality_status, quality_reason) "
            "SELECT instrument_id, provider_id, timeframe, event_time, ingest_time, "
            "       open, high, low, close, volume, price_basis, quality_status, quality_reason "
            "FROM _bars "
            "ON CONFLICT (instrument_id, provider_id, timeframe, price_basis, event_time) "
            "DO UPDATE SET "
            "  open = EXCLUDED.open, high = EXCLUDED.high, "
            "  low = EXCLUDED.low, close = EXCLUDED.close, volume = EXCLUDED.volume, "
            "  quality_status = EXCLUDED.quality_status, "
            "  quality_reason = EXCLUDED.quality_reason, "
            // ingest_time records when this observation was FIRST known here and
            // is never moved forward: the poller re-reads its window every pass,
            // and stamping each re-read as a fresh ingest would make every row
            // look as new as the last poll. That destroys the one honest bound a
            // consumer has on availability when the source states no release time.
            "  ingest_time = macro_price_bars.ingest_time, "
            "  revision = macro_price_bars.revision + 1, "
            "  updated_at = now() "
            // Two rules, and both exist because the poller deliberately re-reads
            // a window the archive may still be writing.
            //
            // A refetch may overwrite only when it carries a price, or when it
            // agrees there is none: a transient MARKET_CLOSED from a provider
            // hiccup must never erase history we already hold.
            //
            // And quality may only improve. The archive fills a day in as ticks
            // arrive, so an early read can legitimately return a carried-forward
            // price for a minute that later fills in with real ones -- STALE is
            // upgraded to VALID as it should be. The reverse never happens
            // honestly, so it is refused: an observation that once traded does
            // not become a carry-forward, and letting it would quietly erase
            // real ticks on every poll.
            "WHERE (EXCLUDED.close IS NOT NULL OR macro_price_bars.close IS NULL) "
            "  AND NOT (macro_price_bars.quality_status = 'VALID' "
            "           AND EXCLUDED.quality_status = 'STALE') "
            "RETURNING EXTRACT(EPOCH FROM event_time)*1000");
        written.clear();
        written.reserve(res.size());
        for (const auto& row : res) {
            written.push_back(static_cast<int64_t>(row[0].as<double>()));
        }
    });
    if (!r) return std::unexpected(r.error());
    return written;
}

Result<std::vector<int64_t>> Store::upsert_reference(
    const SourceBinding& binding, const std::vector<domain::ReferenceValue>& values) {
    std::vector<int64_t> written;
    if (values.empty()) return written;
    auto r = pool_.with_txn([&](pqxx::work& txn) {
        txn.exec("CREATE TEMP TABLE _ref (LIKE macro_reference_values INCLUDING DEFAULTS) "
                 "ON COMMIT DROP");
        {
            pqxx::stream_to stream = pqxx::stream_to::table(
                txn, {"_ref"},
                {"instrument_id", "provider_id", "event_time", "ingest_time", "publication_time",
                 "value", "value_as_of", "quality_status", "quality_reason"});
            const std::string now = ts(timeutil::now_ms());
            for (const auto& v : values) {
                stream << std::make_tuple(
                    binding.instrument_id, binding.provider_id,
                    ts(v.event_time_ms), now,
                    v.publication_time_ms ? std::optional<std::string>{ts(*v.publication_time_ms)}
                                          : std::optional<std::string>{},
                    v.value,
                    v.value_as_of_ms ? std::optional<std::string>{ts(*v.value_as_of_ms)}
                                     : std::optional<std::string>{},
                    std::string(domain::to_string(v.quality)),
                    v.quality_reason.empty() ? std::optional<std::string>{}
                                             : std::optional<std::string>{v.quality_reason});
            }
            stream.complete();
        }
        const auto res = txn.exec(
            "INSERT INTO macro_reference_values "
            "  (instrument_id, provider_id, event_time, ingest_time, publication_time, "
            "   value, value_as_of, quality_status, quality_reason) "
            "SELECT instrument_id, provider_id, event_time, ingest_time, publication_time, "
            "       value, value_as_of, quality_status, quality_reason FROM _ref "
            "ON CONFLICT (instrument_id, provider_id, event_time) DO UPDATE SET "
            "  value = EXCLUDED.value, value_as_of = EXCLUDED.value_as_of, "
            "  publication_time = EXCLUDED.publication_time, "
            "  quality_status = EXCLUDED.quality_status, "
            "  quality_reason = EXCLUDED.quality_reason, "
            // ingest_time records when this observation was FIRST known here and
            // is never moved forward: the poller re-reads its window every pass,
            // and stamping each re-read as a fresh ingest would make every row
            // look as new as the last poll. That destroys the one honest bound a
            // consumer has on availability when the source states no release time.
            "  ingest_time = macro_reference_values.ingest_time, "
            "  revision = macro_reference_values.revision + 1, updated_at = now() "
            "WHERE EXCLUDED.value IS NOT NULL OR macro_reference_values.value IS NULL "
            "RETURNING EXTRACT(EPOCH FROM event_time)*1000");
        written.clear();
        written.reserve(res.size());
        for (const auto& row : res) {
            written.push_back(static_cast<int64_t>(row[0].as<double>()));
        }
    });
    if (!r) return std::unexpected(r.error());
    return written;
}

Result<int64_t> Store::upsert_releases(const SourceBinding& binding,
                                       const std::vector<domain::ReleaseObservation>& obs) {
    if (obs.empty()) return int64_t{0};
    int64_t written = 0;
    auto r = pool_.with_txn([&](pqxx::work& txn) {
        for (const auto& o : obs) {
            // Vintages are few and arrive one snapshot at a time, so the
            // straightforward statement is the right one here.
            const auto res = txn.exec_params(
                "INSERT INTO macro_events "
                "  (instrument_id, provider_id, observation_time, publication_time, "
                "   vintage_date, revision, value, quality_status, quality_reason) "
                "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9) "
                // A vintage is immutable by definition: the archive's answer for
                // a given snapshot date does not change. Re-fetching it is a
                // no-op rather than a rewrite of recorded history.
                "ON CONFLICT (instrument_id, provider_id, observation_time, vintage_date) "
                "DO NOTHING",
                binding.instrument_id, binding.provider_id,
                ts(o.observation_time_ms),
                o.publication_time_ms ? std::optional<std::string>{ts(*o.publication_time_ms)}
                                      : std::optional<std::string>{},
                o.vintage_date, o.revision, o.value,
                std::string(domain::to_string(o.quality)),
                o.quality_reason.empty() ? std::optional<std::string>{}
                                         : std::optional<std::string>{o.quality_reason});
            written += res.affected_rows();
        }
    });
    if (!r) return std::unexpected(r.error());
    return written;
}

Result<std::optional<Cursor>> Store::get_cursor(int64_t instrument_id, int16_t provider_id,
                                                const std::string& timeframe,
                                                domain::PriceBasis basis) {
    std::optional<Cursor> out;
    auto r = pool_.with_txn([&](pqxx::work& txn) {
        const auto rows = txn.exec_params(
            "SELECT EXTRACT(EPOCH FROM watermark)*1000, "
            "       EXTRACT(EPOCH FROM backfilled_from)*1000 "
            "FROM macro_ingest_cursors "
            "WHERE instrument_id=$1 AND provider_id=$2 AND timeframe=$3 AND price_basis=$4",
            instrument_id, provider_id, timeframe, std::string(domain::to_string(basis)));
        if (!rows.empty()) {
            Cursor c;
            c.watermark_ms = static_cast<int64_t>(rows[0][0].as<double>());
            if (!rows[0][1].is_null()) {
                c.backfilled_from_ms = static_cast<int64_t>(rows[0][1].as<double>());
            }
            out = c;
        }
    });
    if (!r) return std::unexpected(r.error());
    return out;
}

Result<void> Store::set_cursor(int64_t instrument_id, int16_t provider_id,
                               const std::string& timeframe, domain::PriceBasis basis,
                               int64_t watermark_ms, std::optional<int64_t> backfilled_from_ms,
                               const std::string& last_error) {
    return pool_.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "INSERT INTO macro_ingest_cursors "
            "  (instrument_id, provider_id, timeframe, price_basis, watermark, backfilled_from, "
            "   last_run_at, last_error) "
            "VALUES ($1,$2,$3,$4,$5,$6,now(),$7) "
            "ON CONFLICT (instrument_id, provider_id, timeframe, price_basis) DO UPDATE SET "
            // The watermark only ever moves forward. A retry that starts from
            // an earlier point must not drag the resume position backwards and
            // cause the range in between to be walked twice.
            "  watermark = GREATEST(macro_ingest_cursors.watermark, EXCLUDED.watermark), "
            "  backfilled_from = LEAST(COALESCE(macro_ingest_cursors.backfilled_from, "
            "                                   EXCLUDED.backfilled_from), "
            "                          COALESCE(EXCLUDED.backfilled_from, "
            "                                   macro_ingest_cursors.backfilled_from)), "
            "  last_run_at = now(), last_error = EXCLUDED.last_error, updated_at = now()",
            instrument_id, provider_id, timeframe, std::string(domain::to_string(basis)),
            ts(watermark_ms),
            backfilled_from_ms ? std::optional<std::string>{ts(*backfilled_from_ms)}
                               : std::optional<std::string>{},
            last_error.empty() ? std::optional<std::string>{} : std::optional<std::string>{last_error});
    });
}

Result<RunHandle> Store::begin_run(int16_t provider_id, std::optional<int64_t> instrument_id,
                                   const std::string& kind, std::optional<int64_t> range_start_ms,
                                   std::optional<int64_t> range_end_ms) {
    RunHandle h;
    auto r = pool_.with_txn([&](pqxx::work& txn) {
        const auto rows = txn.exec_params(
            "INSERT INTO macro_ingest_runs (provider_id, instrument_id, kind, range_start, range_end) "
            "VALUES ($1,$2,$3,$4,$5) RETURNING id",
            provider_id, instrument_id, kind,
            range_start_ms ? std::optional<std::string>{ts(*range_start_ms)}
                           : std::optional<std::string>{},
            range_end_ms ? std::optional<std::string>{ts(*range_end_ms)}
                         : std::optional<std::string>{});
        h.id = rows[0][0].as<int64_t>();
    });
    if (!r) return std::unexpected(r.error());
    return h;
}

Result<void> Store::finish_run(RunHandle handle, const std::string& outcome, int64_t rows_written,
                               std::optional<int> http_status, const std::string& detail) {
    return pool_.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "UPDATE macro_ingest_runs SET finished_at=now(), outcome=$2, rows_written=$3, "
            "  http_status=$4, detail=$5 WHERE id=$1",
            handle.id, outcome, static_cast<int>(rows_written), http_status,
            detail.empty() ? std::optional<std::string>{} : std::optional<std::string>{detail});
    });
}

Result<void> Store::observe_coverage(int64_t instrument_id, std::optional<int64_t> earliest_ms,
                                     std::optional<int64_t> latest_ms) {
    return pool_.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "UPDATE macro_instruments SET "
            // LEAST/GREATEST with COALESCE so the first observation sets the
            // bound and later ones only widen it. These fields record what was
            // actually served, so they must never narrow on a partial fetch.
            "  earliest_available_timestamp = LEAST("
            "     COALESCE(earliest_available_timestamp, $2::timestamptz), $2::timestamptz), "
            "  latest_observed_timestamp = GREATEST("
            "     COALESCE(latest_observed_timestamp, $3::timestamptz), $3::timestamptz), "
            "  updated_at = now() "
            "WHERE id=$1",
            instrument_id,
            earliest_ms ? std::optional<std::string>{ts(*earliest_ms)} : std::optional<std::string>{},
            latest_ms ? std::optional<std::string>{ts(*latest_ms)} : std::optional<std::string>{});
    });
}

Result<void> Store::record_gap(int64_t instrument_id, int16_t provider_id,
                               const std::string& timeframe, int64_t gap_start_ms,
                               int64_t gap_end_ms, std::optional<int> expected_points,
                               const std::string& resolution) {
    return pool_.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "INSERT INTO macro_series_gaps "
            "  (instrument_id, provider_id, timeframe, gap_start, gap_end, expected_points, "
            "   resolution, resolved_at) "
            "VALUES ($1,$2,$3,$4,$5,$6,$7, CASE WHEN $7='unresolved' THEN NULL ELSE now() END) "
            "ON CONFLICT (instrument_id, provider_id, timeframe, gap_start) DO UPDATE SET "
            "  gap_end = EXCLUDED.gap_end, expected_points = EXCLUDED.expected_points, "
            // Two subsystems write this row and they do not know the same
            // things. The loader was there when the request was made and knows
            // WHY the period is empty -- the archive answered 404, or served an
            // empty file. The scanner only knows that rows are absent. Letting
            // the scanner's generic 'unresolved' overwrite the loader's
            // specific finding throws away the only evidence anyone has.
            "  resolution = CASE WHEN EXCLUDED.resolution = 'unresolved' "
            "                      AND macro_series_gaps.resolution <> 'unresolved' "
            "                    THEN macro_series_gaps.resolution "
            "                    ELSE EXCLUDED.resolution END, "
            "  resolved_at = CASE WHEN EXCLUDED.resolution='unresolved' THEN NULL "
            "                      ELSE now() END",
            instrument_id, provider_id, timeframe, ts(gap_start_ms), ts(gap_end_ms),
            expected_points, resolution);
    });
}

Result<void> Store::note_provider_attempt(int16_t provider_id, bool success,
                                          const std::string& failure_reason) {
    return pool_.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "UPDATE macro_provider_metadata SET "
            "  last_attempt_at = now(), "
            "  last_success_at = CASE WHEN $2 THEN now() ELSE last_success_at END, "
            "  last_failure_at = CASE WHEN $2 THEN last_failure_at ELSE now() END, "
            "  last_failure_reason = CASE WHEN $2 THEN last_failure_reason ELSE $3 END, "
            "  consecutive_failures = CASE WHEN $2 THEN 0 ELSE consecutive_failures + 1 END, "
            "  requests_today = CASE WHEN requests_today_date = CURRENT_DATE "
            "                        THEN requests_today + 1 ELSE 1 END, "
            "  requests_today_date = CURRENT_DATE, "
            "  updated_at = now() "
            "WHERE provider_id = $1",
            provider_id, success,
            failure_reason.empty() ? std::optional<std::string>{}
                                   : std::optional<std::string>{failure_reason.substr(0, 500)});
    });
}

Result<int> Store::ensure_partitions(int64_t from_ms, int64_t to_ms) {
    int created = 0;
    auto r = pool_.with_txn([&](pqxx::work& txn) {
        for (int64_t m = month_start_ms(from_ms); m <= to_ms; m = next_month_ms(m)) {
            const std::string suffix = month_key(m);
            const std::string name = "macro_price_bars_" + suffix;
            const auto exists = txn.exec_params(
                "SELECT 1 FROM pg_class WHERE relname = $1", name);
            if (!exists.empty()) continue;
            txn.exec("CREATE TABLE " + txn.quote_name(name) +
                     " PARTITION OF macro_price_bars FOR VALUES FROM (" +
                     txn.quote(ts(m)) + ") TO (" + txn.quote(ts(next_month_ms(m))) + ")");
            ++created;
        }
    });
    if (!r) return std::unexpected(r.error());
    return created;
}

Result<std::map<int64_t, int64_t>>
Store::per_day_bar_counts(int64_t instrument_id, int16_t provider_id, const std::string& timeframe,
                          domain::PriceBasis basis, int64_t from_ms, int64_t to_ms) {
    std::map<int64_t, int64_t> out;
    auto r = pool_.with_txn([&](pqxx::work& txn) {
        const auto rows = txn.exec_params(
            "SELECT EXTRACT(EPOCH FROM date_trunc('day', event_time))*1000 AS day, count(*) "
            "FROM macro_price_bars "
            "WHERE instrument_id=$1 AND provider_id=$2 AND timeframe=$3 AND price_basis=$4 "
            "  AND event_time >= $5 AND event_time < $6 "
            "GROUP BY 1",
            instrument_id, provider_id, timeframe, std::string(domain::to_string(basis)),
            ts(from_ms), ts(to_ms));
        for (const auto& row : rows) {
            out[static_cast<int64_t>(row[0].as<double>())] = row[1].as<int64_t>();
        }
    });
    if (!r) return std::unexpected(r.error());
    return out;
}

Result<std::vector<Store::CoverageRow>> Store::coverage_report() {
    std::vector<CoverageRow> out;
    auto r = pool_.with_txn([&](pqxx::work& txn) {
        const auto bars = txn.exec(
            "SELECT i.code, p.code, b.timeframe, b.price_basis, count(*), "
            "  count(*) FILTER (WHERE b.quality_status='VALID'), "
            "  count(*) FILTER (WHERE b.quality_status='MARKET_CLOSED'), "
            "  count(*) FILTER (WHERE b.quality_status='MISSING'), "
            "  EXTRACT(EPOCH FROM min(b.event_time))*1000, "
            "  EXTRACT(EPOCH FROM max(b.event_time))*1000 "
            "FROM macro_price_bars b "
            "JOIN macro_instruments i ON i.id=b.instrument_id "
            "JOIN macro_providers p ON p.id=b.provider_id "
            "GROUP BY 1,2,3,4 ORDER BY 1,4");
        for (const auto& row : bars) {
            CoverageRow c;
            c.instrument_code = row[0].as<std::string>();
            c.provider_code = row[1].as<std::string>();
            c.timeframe = row[2].as<std::string>();
            c.price_basis = row[3].as<std::string>();
            c.rows = row[4].as<int64_t>();
            c.valid = row[5].as<int64_t>();
            c.market_closed = row[6].as<int64_t>();
            c.missing = row[7].as<int64_t>();
            if (!row[8].is_null()) c.earliest_ms = static_cast<int64_t>(row[8].as<double>());
            if (!row[9].is_null()) c.latest_ms = static_cast<int64_t>(row[9].as<double>());
            out.push_back(std::move(c));
        }
        const auto refs = txn.exec(
            "SELECT i.code, p.code, 'daily', 'reference', count(*), "
            "  count(*) FILTER (WHERE v.quality_status='VALID'), 0::bigint, "
            "  count(*) FILTER (WHERE v.quality_status='MISSING'), "
            "  EXTRACT(EPOCH FROM min(v.event_time))*1000, "
            "  EXTRACT(EPOCH FROM max(v.event_time))*1000 "
            "FROM macro_reference_values v "
            "JOIN macro_instruments i ON i.id=v.instrument_id "
            "JOIN macro_providers p ON p.id=v.provider_id "
            "GROUP BY 1,2 ORDER BY 1");
        for (const auto& row : refs) {
            CoverageRow c;
            c.instrument_code = row[0].as<std::string>();
            c.provider_code = row[1].as<std::string>();
            c.timeframe = row[2].as<std::string>();
            c.price_basis = row[3].as<std::string>();
            c.rows = row[4].as<int64_t>();
            c.valid = row[5].as<int64_t>();
            c.market_closed = row[6].as<int64_t>();
            c.missing = row[7].as<int64_t>();
            if (!row[8].is_null()) c.earliest_ms = static_cast<int64_t>(row[8].as<double>());
            if (!row[9].is_null()) c.latest_ms = static_cast<int64_t>(row[9].as<double>());
            out.push_back(std::move(c));
        }
    });
    if (!r) return std::unexpected(r.error());
    return out;
}

} // namespace altmacro::data
