#pragma once

#include "altmacro/core/config.hpp"
#include "altmacro/core/error.hpp"
#include "altmacro/data/store.hpp"
#include "altmacro/providers/registry.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace altmacro::components {

// Historical load.
//
// The target window is AltSpotter's real 1m history, so that external series
// cover the same ground as the crypto they will be studied against. Where a
// source reaches less far, that is recorded as reduced coverage; nothing is
// generated to make a range look complete.
//
// Progress is durable. The cursor is advanced only after the rows it covers
// are committed, so an interrupted run resumes from the last committed chunk
// and never skips the range in between. Re-running a completed window is safe
// and writes nothing new, because every write is an upsert on the
// observation's own identity.
class Bootstrap {
public:
    Bootstrap(const Config& cfg, providers::Registry& registry, data::Store& store)
        : cfg_(cfg), registry_(registry), store_(store) {}

    struct InstrumentResult {
        std::string instrument_code;
        std::string provider_code;
        std::string price_basis;
        int64_t rows_written = 0;
        int64_t chunks_done = 0;
        int64_t chunks_closed = 0;      // periods the source said were shut
        int64_t rejected_rows = 0;
        int64_t stale_rows = 0;
        std::optional<int64_t> earliest_ms;
        std::optional<int64_t> latest_ms;
        std::string outcome;            // ok | partial | failed | deferred
        std::string detail;
    };

    struct Summary {
        std::vector<InstrumentResult> results;
        int64_t total_rows = 0;
        bool interrupted = false;

        // True when every series the registry could serve reached the end of
        // the requested window. A run that left one series a third loaded must
        // not exit reporting success: systemd would record it as a clean
        // one-shot and nobody would look again.
        [[nodiscard]] bool complete() const {
            for (const auto& r : results) {
                if (r.outcome == "failed" || r.outcome == "partial") return false;
            }
            return !interrupted;
        }
    };

    [[nodiscard]] Result<Summary> run(const std::function<bool()>& should_stop);

    // Where a walk should begin, given the requested window start and whatever
    // cursor survives from previous runs.
    //
    // Pure, and separated out because this decision has been wrong twice. A
    // watermark says how far a walk got; `backfilled_from` says how far back it
    // ever started. Only the pair licenses a skip: trusting the watermark alone
    // makes a widened window silently load nothing, and the resulting series
    // looks complete.
    struct ResumeDecision {
        int64_t start_ms = 0;
        bool resumed = false;      // continued from a watermark
        bool restarted = false;    // ignored a watermark because the window grew
    };
    [[nodiscard]] static ResumeDecision
    resolve_resume_start(int64_t requested_from_ms, const std::optional<data::Cursor>& stored);

private:
    const Config& cfg_;
    providers::Registry& registry_;
    data::Store& store_;

    [[nodiscard]] Result<InstrumentResult>
    load_market_series(const data::SourceBinding& b, const std::string& timeframe,
                       int64_t from_ms, int64_t to_ms, const std::function<bool()>& should_stop);

    [[nodiscard]] Result<InstrumentResult>
    load_reference_series(const data::SourceBinding& b, int64_t from_ms, int64_t to_ms,
                          const std::function<bool()>& should_stop);
};

} // namespace altmacro::components
