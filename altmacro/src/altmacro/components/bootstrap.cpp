#include "altmacro/components/bootstrap.hpp"

#include "altmacro/core/logger.hpp"
#include "altmacro/core/time.hpp"
#include "altmacro/domain/validation.hpp"

#include <algorithm>

namespace altmacro::components {
namespace {

constexpr int64_t kDayMs = 86'400'000;

domain::SanityBand band_of(const data::SourceBinding& b) {
    domain::SanityBand band;
    if (b.sanity_min && b.sanity_max) { band.min = *b.sanity_min; band.max = *b.sanity_max; }
    return band;
}

bool is_market_series(const data::SourceBinding& b) { return b.series_class == "MARKET"; }

} // namespace

Bootstrap::ResumeDecision
Bootstrap::resolve_resume_start(int64_t requested_from_ms,
                                const std::optional<data::Cursor>& stored) {
    ResumeDecision d;
    d.start_ms = requested_from_ms;
    if (!stored.has_value()) return d;

    // `backfilled_from` records where a walk actually began, not where it was
    // asked to begin. Without that distinction a resumed run would record the
    // configured start as covered ground and the next run would believe it.
    const bool covers_requested_start =
        stored->backfilled_from_ms.has_value() &&
        *stored->backfilled_from_ms <= requested_from_ms;

    if (covers_requested_start && stored->watermark_ms > requested_from_ms) {
        d.start_ms = stored->watermark_ms;
        d.resumed = true;
    } else if (!covers_requested_start) {
        // Re-walking is safe: every write is an upsert on the observation's own
        // identity, so the cost is requests, never duplicated rows.
        d.restarted = true;
    }
    return d;
}

Result<Bootstrap::Summary> Bootstrap::run(const std::function<bool()>& should_stop) {
    const auto start = timeutil::parse_iso8601(cfg_.bootstrap.history_start);
    if (!start) {
        return fail(ErrorCode::config_error,
                    "bootstrap.history_start is not an ISO-8601 instant: " +
                        cfg_.bootstrap.history_start);
    }
    int64_t end = timeutil::now_ms();
    if (!cfg_.bootstrap.history_end.empty()) {
        const auto parsed = timeutil::parse_iso8601(cfg_.bootstrap.history_end);
        if (!parsed) {
            return fail(ErrorCode::config_error,
                        "bootstrap.history_end is not an ISO-8601 instant: " +
                            cfg_.bootstrap.history_end);
        }
        end = *parsed;
    }
    if (end <= *start) {
        return fail(ErrorCode::config_error, "bootstrap window ends before it begins");
    }

    // Partitions must exist before the first row lands, and they are created
    // for the whole window up front rather than lazily.
    auto parts = store_.ensure_partitions(*start, end + 31 * kDayMs);
    if (!parts) return std::unexpected(parts.error());
    log::info("partitions_ensured", {{"created", *parts}});

    Summary summary;
    // Instruments the registry could not serve are reported before any work
    // starts, so a missing series is visible immediately rather than inferred
    // later from an empty table.
    for (const auto& r : registry_.rejected()) {
        log::warn("binding_not_served", {{"instrument", r.instrument_code},
                                         {"provider", r.provider_code}, {"reason", r.reason}});
        InstrumentResult ir;
        ir.instrument_code = r.instrument_code;
        ir.provider_code = r.provider_code;
        ir.outcome = "deferred";
        ir.detail = r.reason;
        summary.results.push_back(std::move(ir));
    }

    for (const auto& b : registry_.active()) {
        if (should_stop && should_stop()) { summary.interrupted = true; break; }

        Result<InstrumentResult> res = is_market_series(b)
            ? load_market_series(b, "1m", *start, end, should_stop)
            : load_reference_series(b, *start, end, should_stop);

        if (!res) {
            // One failing source must not stop the others: the directive is
            // explicit that a single missing provider cannot block the system.
            log::error("series_load_failed",
                       {{"instrument", b.instrument_code}, {"provider", b.provider_code},
                        {"error", res.error().message}, {"code", res.error().code_name()}});
            InstrumentResult ir;
            ir.instrument_code = b.instrument_code;
            ir.provider_code = b.provider_code;
            ir.price_basis = domain::to_string(b.price_basis);
            ir.outcome = "failed";
            ir.detail = res.error().message;
            summary.results.push_back(std::move(ir));
            continue;
        }
        summary.total_rows += res->rows_written;
        summary.results.push_back(std::move(*res));
    }

    return summary;
}

Result<Bootstrap::InstrumentResult>
Bootstrap::load_market_series(const data::SourceBinding& b, const std::string& timeframe,
                              int64_t from_ms, int64_t to_ms,
                              const std::function<bool()>& should_stop) {
    InstrumentResult out;
    out.instrument_code = b.instrument_code;
    out.provider_code = b.provider_code;
    out.price_basis = domain::to_string(b.price_basis);
    out.outcome = "ok";

    auto provider = registry_.get(b.provider_code);
    if (!provider) {
        return fail(ErrorCode::config_error, "provider vanished from the registry mid-run");
    }

    int64_t cursor_ms = from_ms;
    if (cfg_.bootstrap.resume) {
        auto c = store_.get_cursor(b.instrument_id, b.provider_id, timeframe, b.price_basis);
        if (!c) return std::unexpected(c.error());
        const auto decision = resolve_resume_start(from_ms, *c);
        cursor_ms = decision.start_ms;
        if (decision.resumed) {
            log::info("bootstrap_resumed",
                      {{"instrument", b.instrument_code}, {"provider", b.provider_code},
                       {"basis", out.price_basis},
                       {"from", timeutil::to_iso8601(cursor_ms)}});
        } else if (decision.restarted) {
            log::info("bootstrap_window_widened",
                      {{"instrument", b.instrument_code}, {"provider", b.provider_code},
                       {"basis", out.price_basis},
                       {"requested_start", timeutil::to_iso8601(from_ms)},
                       {"cursor_reached_back_to",
                        (*c)->backfilled_from_ms
                            ? timeutil::to_iso8601(*(*c)->backfilled_from_ms)
                            : std::string("never recorded")},
                       {"action", "restarting from the requested start rather than skipping"}});
        }
    }

    if (cursor_ms >= to_ms) {
        out.detail = "already covered to the end of the window";
        return out;
    }

    // Where this walk ACTUALLY begins, which is not necessarily the configured
    // window start: a resume begins at the watermark. Recording the configured
    // start instead would make the cursor claim coverage it never walked, and a
    // later run would trust that claim and skip the range for good. That is
    // exactly how nine months went missing in testing while the series looked
    // complete.
    const int64_t walk_start_ms = cursor_ms;

    providers::InstrumentDescriptor desc;
    desc.native_symbol = b.native_symbol;
    desc.display_name = b.instrument_code;
    desc.price_basis = b.price_basis;
    desc.series_class = providers::SeriesClass::market;

    const auto band = band_of(b);
    auto run = store_.begin_run(b.provider_id, b.instrument_id, "bootstrap", cursor_ms, to_ms);
    if (!run) return std::unexpected(run.error());

    std::vector<domain::PriceBar> pending;
    pending.reserve(static_cast<size_t>(cfg_.bootstrap.batch_rows) + 1500);

    // Flushes the accumulated rows and advances the cursor in that order. The
    // cursor must never move ahead of durable data: if it did, a crash between
    // the two would leave a range that the resume path believes is complete.
    auto flush = [&](int64_t new_watermark) -> Result<void> {
        if (!pending.empty()) {
            auto n = store_.upsert_bars(b, timeframe, pending);
            if (!n) return std::unexpected(n.error());
            out.rows_written += static_cast<int64_t>(n->size());
            pending.clear();
        }
        auto c = store_.set_cursor(b.instrument_id, b.provider_id, timeframe, b.price_basis,
                                   new_watermark, walk_start_ms, "");
        if (!c) return std::unexpected(c.error());
        return {};
    };

    while (cursor_ms < to_ms) {
        if (should_stop && should_stop()) {
            auto f = flush(cursor_ms);
            if (!f) return std::unexpected(f.error());
            out.outcome = "partial";
            out.detail = "interrupted by shutdown";
            auto fin = store_.finish_run(*run, "aborted", out.rows_written, std::nullopt, out.detail);
            if (!fin) return std::unexpected(fin.error());
            return out;
        }

        providers::Window w{cursor_ms, to_ms};
        auto page = provider->fetch_historical_bars(desc, timeframe, w, should_stop);
        if (!page) {
            auto note = store_.note_provider_attempt(b.provider_id, false, page.error().message);
            (void)note;
            // Commit what is already in hand before giving up, so the work done
            // so far is not lost and the resume point is honest.
            auto f = flush(cursor_ms);
            if (!f) return std::unexpected(f.error());
            auto fin = store_.finish_run(*run, "provider_error", out.rows_written, std::nullopt,
                                         page.error().message);
            if (!fin) return std::unexpected(fin.error());
            if (page.error().code == ErrorCode::shutdown_requested) {
                out.outcome = "partial";
                out.detail = "interrupted by shutdown";
                return out;
            }
            return std::unexpected(page.error());
        }
        auto note = store_.note_provider_attempt(b.provider_id, true, "");
        (void)note;

        const int64_t chunk_start = cursor_ms;
        const int64_t next = page->next_cursor_ms.value_or(to_ms);

        if (page->bars.empty()) {
            ++out.chunks_closed;
            // Record WHY the period is empty, not merely that it is. A shut
            // market, an archive that has not caught up, and a period the
            // source genuinely lacks are three different facts, and a consumer
            // that cannot tell them apart will read the second as the first.
            const int64_t gap_end = std::min(next, to_ms);
            const char* resolution = "unresolved";
            switch (page->absence) {
                case providers::AbsenceReason::market_closed:
                    resolution = "market_closed";
                    break;
                case providers::AbsenceReason::not_in_archive_yet:
                    // Not a defect and not a closure: the live poller's lookback
                    // will fill it once the source writes it. It stays
                    // unresolved so that it remains visible until it does.
                    resolution = "provider_delay";
                    break;
                case providers::AbsenceReason::absent_at_source:
                    resolution = "confirmed_absent_at_source";
                    break;
                case providers::AbsenceReason::none:
                    resolution = "unresolved";
                    break;
            }
            auto g = store_.record_gap(b.instrument_id, b.provider_id, timeframe, chunk_start,
                                       gap_end, std::nullopt, resolution);
            if (!g) return std::unexpected(g.error());

            if (page->absence == providers::AbsenceReason::not_in_archive_yet) {
                // Stop here rather than stepping over it. This period WILL have
                // data once the source writes it, and the watermark is a promise
                // that everything behind it has been collected -- advancing past
                // a period that is merely late turns that promise into a lie no
                // later run can detect, because the next run starts after the
                // hole.
                //
                // This is not hypothetical: the first full load hit a 404 on the
                // day it ran, advanced over it, and left three series missing
                // 00:00-21:16 of that day with the watermark claiming otherwise.
                auto f = flush(chunk_start);
                if (!f) return std::unexpected(f.error());
                out.outcome = "partial";
                out.detail = "stopped at " + timeutil::to_iso8601(chunk_start) +
                             ": the source has not archived this period yet";
                log::info("bootstrap_stopped_at_archive_edge",
                          {{"instrument", b.instrument_code}, {"provider", b.provider_code},
                           {"basis", out.price_basis},
                           {"at", timeutil::to_iso8601(chunk_start)}});
                auto fin = store_.finish_run(*run, "ok", out.rows_written, std::nullopt,
                                             out.detail);
                if (!fin) return std::unexpected(fin.error());
                if (out.earliest_ms || out.latest_ms) {
                    auto cov = store_.observe_coverage(b.instrument_id, out.earliest_ms,
                                                       out.latest_ms);
                    if (!cov) return std::unexpected(cov.error());
                }
                return out;
            }
        } else {
            auto bars = page->bars;
            const auto report = domain::validate_bars(bars, band);
            out.rejected_rows += report.rejected;
            out.stale_rows += report.stale;
            if (!report.clean()) {
                // Rejections are loud. A wrong decode scale surfaces here, and
                // it is the one failure that would otherwise be invisible.
                log::error("bars_rejected",
                           {{"instrument", b.instrument_code}, {"provider", b.provider_code},
                            {"basis", out.price_basis}, {"rejected", report.rejected},
                            {"checked", report.checked},
                            {"first_rule", report.issues.empty() ? "" : report.issues.front().rule},
                            {"first_detail",
                             report.issues.empty() ? "" : report.issues.front().detail}});
            }
            if (!bars.empty()) {
                if (!out.earliest_ms || bars.front().event_time_ms < *out.earliest_ms) {
                    out.earliest_ms = bars.front().event_time_ms;
                }
                if (!out.latest_ms || bars.back().event_time_ms > *out.latest_ms) {
                    out.latest_ms = bars.back().event_time_ms;
                }
                pending.insert(pending.end(), std::make_move_iterator(bars.begin()),
                               std::make_move_iterator(bars.end()));
            }
        }

        ++out.chunks_done;
        cursor_ms = next;

        // Bounded memory: the batch is flushed as soon as it reaches its size,
        // never accumulated across the whole window.
        if (static_cast<int32_t>(pending.size()) >= cfg_.bootstrap.batch_rows) {
            auto f = flush(cursor_ms);
            if (!f) return std::unexpected(f.error());
        }
        if (!page->next_cursor_ms) break;
    }

    auto f = flush(std::min(cursor_ms, to_ms));
    if (!f) return std::unexpected(f.error());

    if (out.earliest_ms || out.latest_ms) {
        auto cov = store_.observe_coverage(b.instrument_id, out.earliest_ms, out.latest_ms);
        if (!cov) return std::unexpected(cov.error());
    }
    auto fin = store_.finish_run(*run, out.rows_written > 0 ? "ok" : "empty", out.rows_written,
                                 std::nullopt, out.detail);
    if (!fin) return std::unexpected(fin.error());

    log::info("market_series_loaded",
              {{"instrument", b.instrument_code}, {"provider", b.provider_code},
               {"basis", out.price_basis}, {"rows", out.rows_written},
               {"chunks", out.chunks_done}, {"closed_periods", out.chunks_closed},
               {"stale_rows", out.stale_rows}, {"rejected", out.rejected_rows}});
    return out;
}

Result<Bootstrap::InstrumentResult>
Bootstrap::load_reference_series(const data::SourceBinding& b, int64_t from_ms, int64_t to_ms,
                                 const std::function<bool()>& should_stop) {
    InstrumentResult out;
    out.instrument_code = b.instrument_code;
    out.provider_code = b.provider_code;
    out.price_basis = domain::to_string(b.price_basis);
    out.outcome = "ok";

    auto provider = registry_.get(b.provider_code);
    if (!provider) {
        return fail(ErrorCode::config_error, "provider vanished from the registry mid-run");
    }

    providers::InstrumentDescriptor desc;
    desc.native_symbol = b.native_symbol;
    desc.display_name = b.instrument_code;
    desc.price_basis = b.price_basis;
    desc.series_class = providers::SeriesClass::official_reference;

    auto run = store_.begin_run(b.provider_id, b.instrument_id, "bootstrap", from_ms, to_ms);
    if (!run) return std::unexpected(run.error());

    // A daily series for this window is a few hundred points: one request, one
    // transaction. Chunking it would multiply requests against a published
    // rate limit for no benefit.
    providers::Window w{from_ms, to_ms};
    auto page = provider->fetch_historical_reference(desc, w, should_stop);
    if (!page) {
        auto note = store_.note_provider_attempt(b.provider_id, false, page.error().message);
        (void)note;
        auto fin = store_.finish_run(*run, "provider_error", 0, std::nullopt, page.error().message);
        if (!fin) return std::unexpected(fin.error());
        return std::unexpected(page.error());
    }
    auto note = store_.note_provider_attempt(b.provider_id, true, "");
    (void)note;

    auto values = page->values;
    const auto band = band_of(b);
    const auto report = domain::validate_reference(values, band);
    out.rejected_rows = report.rejected;
    if (!report.clean()) {
        log::error("reference_rejected",
                   {{"instrument", b.instrument_code}, {"provider", b.provider_code},
                    {"rejected", report.rejected}, {"checked", report.checked},
                    {"first_rule", report.issues.empty() ? "" : report.issues.front().rule},
                    {"first_detail", report.issues.empty() ? "" : report.issues.front().detail}});
    }

    if (!values.empty()) {
        out.earliest_ms = values.front().event_time_ms;
        out.latest_ms = values.back().event_time_ms;
        auto n = store_.upsert_reference(b, values);
        if (!n) return std::unexpected(n.error());
        out.rows_written = static_cast<int64_t>(n->size());
        auto cov = store_.observe_coverage(b.instrument_id, out.earliest_ms, out.latest_ms);
        if (!cov) return std::unexpected(cov.error());
    } else {
        out.outcome = "partial";
        out.detail = "source served no observations inside the requested window";
    }

    auto c = store_.set_cursor(b.instrument_id, b.provider_id, "1d", b.price_basis, to_ms,
                               from_ms, "");
    if (!c) return std::unexpected(c.error());
    auto fin = store_.finish_run(*run, out.rows_written > 0 ? "ok" : "empty", out.rows_written,
                                 std::nullopt, out.detail);
    if (!fin) return std::unexpected(fin.error());

    log::info("reference_series_loaded",
              {{"instrument", b.instrument_code}, {"provider", b.provider_code},
               {"rows", out.rows_written}, {"rejected", out.rejected_rows}});
    return out;
}

} // namespace altmacro::components
