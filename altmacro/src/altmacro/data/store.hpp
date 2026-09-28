#pragma once

#include "altmacro/core/error.hpp"
#include "altmacro/core/pg.hpp"
#include "altmacro/domain/series.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace altmacro::data {

// A registry row joined across instrument and source: everything needed to
// fetch and store one series, resolved once at startup.
struct SourceBinding {
    int64_t instrument_id = 0;
    int16_t provider_id = 0;
    std::string instrument_code;
    std::string provider_code;
    std::string native_symbol;
    std::string series_class;
    std::string denomination;
    std::string native_frequency;
    std::string session_calendar;
    domain::PriceBasis price_basis = domain::PriceBasis::not_applicable;
    std::string timestamp_semantics;
    std::string source_timezone;
    int16_t priority = 1;
    std::optional<double> decode_scale;
    std::optional<double> sanity_min;
    std::optional<double> sanity_max;
    bool enabled = true;
};

struct Cursor {
    int64_t watermark_ms = 0;
    std::optional<int64_t> backfilled_from_ms;
};

struct RunHandle {
    int64_t id = 0;
};

// PostgreSQL persistence.
//
// Two properties are load-bearing here and are enforced by the schema rather
// than by care:
//
//   * A re-run of the same window must not duplicate anything. Every write is
//     an upsert keyed on the natural identity of the observation, so the
//     bootstrap is safe to interrupt and repeat.
//   * A batch is bounded. The bootstrap covers 283 days of minute bars across
//     four instruments and two sides of the book, which is millions of rows;
//     they are streamed in batches and never accumulated in memory.
class Store {
public:
    explicit Store(persistence::PgPool& pool) : pool_(pool) {}

    [[nodiscard]] Result<std::vector<SourceBinding>> load_bindings();

    // Upsert a batch of bars for one binding. Returns rows written.
    //
    // The conflict target is the observation's identity, and the update only
    // moves a row forward: a later fetch that carries a real price replaces an
    // earlier MARKET_CLOSED marker, but a fetch that carries nothing never
    // erases a price we already hold.
    // Returns the event times that were actually inserted or changed.
    //
    // The poller deliberately re-reads a window the archive may still be
    // writing, so most of what it hands in is already stored unchanged.
    // Publishing the whole window every pass would lean entirely on the
    // broker's deduplication window to suppress the repeats -- and anything
    // that aged out of that window would go over the wire a second time as
    // though it were new. Telling the caller what actually changed removes the
    // question.
    [[nodiscard]] Result<std::vector<int64_t>>
    upsert_bars(const SourceBinding& binding, const std::string& timeframe,
                const std::vector<domain::PriceBar>& bars);

    [[nodiscard]] Result<std::vector<int64_t>>
    upsert_reference(const SourceBinding& binding,
                     const std::vector<domain::ReferenceValue>& values);

    [[nodiscard]] Result<int64_t>
    upsert_releases(const SourceBinding& binding,
                    const std::vector<domain::ReleaseObservation>& observations);

    // Bootstrap progress. The watermark is exclusive: everything strictly
    // before it has been fetched AND committed, including the explicit
    // MARKET_CLOSED and MISSING markers. It is advanced only after the rows it
    // covers are durable, so a crash costs at most one chunk and never leaves a
    // hole the resume path would skip over.
    // `basis` is part of the key: bid and ask come from separate files and are
    // separate series, so they must not share a watermark. It is a required
    // argument rather than an optional one precisely so that a caller cannot
    // omit it and silently make two series into one.
    [[nodiscard]] Result<std::optional<Cursor>>
    get_cursor(int64_t instrument_id, int16_t provider_id, const std::string& timeframe,
               domain::PriceBasis basis);

    [[nodiscard]] Result<void>
    set_cursor(int64_t instrument_id, int16_t provider_id, const std::string& timeframe,
               domain::PriceBasis basis, int64_t watermark_ms,
               std::optional<int64_t> backfilled_from_ms, const std::string& last_error);

    [[nodiscard]] Result<RunHandle>
    begin_run(int16_t provider_id, std::optional<int64_t> instrument_id, const std::string& kind,
              std::optional<int64_t> range_start_ms, std::optional<int64_t> range_end_ms);

    [[nodiscard]] Result<void>
    finish_run(RunHandle handle, const std::string& outcome, int64_t rows_written,
               std::optional<int> http_status, const std::string& detail);

    // Records what the source actually served, so a coverage claim is evidence.
    [[nodiscard]] Result<void>
    observe_coverage(int64_t instrument_id, std::optional<int64_t> earliest_ms,
                     std::optional<int64_t> latest_ms);

    [[nodiscard]] Result<void>
    record_gap(int64_t instrument_id, int16_t provider_id, const std::string& timeframe,
               int64_t gap_start_ms, int64_t gap_end_ms, std::optional<int> expected_points,
               const std::string& resolution);

    [[nodiscard]] Result<void>
    note_provider_attempt(int16_t provider_id, bool success, const std::string& failure_reason);

    // Partition maintenance for macro_price_bars. Called at startup and then
    // hourly from the live loop: the stack has already lost a night to a
    // service that only advanced its partitions when it started.
    [[nodiscard]] Result<int> ensure_partitions(int64_t from_ms, int64_t to_ms);

    struct CoverageRow {
        std::string instrument_code;
        std::string provider_code;
        std::string timeframe;
        std::string price_basis;
        int64_t rows = 0;
        int64_t valid = 0;
        int64_t market_closed = 0;
        int64_t missing = 0;
        std::optional<int64_t> earliest_ms;
        std::optional<int64_t> latest_ms;
    };
    [[nodiscard]] Result<std::vector<CoverageRow>> coverage_report();

    // Rows stored per UTC day for one series, keyed by the day's start in ms.
    // Quality is deliberately NOT filtered: a day is complete at 1440 records
    // whatever they say, because the MARKET_CLOSED markers are part of the day
    // rather than holes in it.
    [[nodiscard]] Result<std::map<int64_t, int64_t>>
    per_day_bar_counts(int64_t instrument_id, int16_t provider_id, const std::string& timeframe,
                       domain::PriceBasis basis, int64_t from_ms, int64_t to_ms);

private:
    persistence::PgPool& pool_;
};

} // namespace altmacro::data
