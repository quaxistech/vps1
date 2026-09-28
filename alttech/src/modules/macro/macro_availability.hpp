#pragma once

#include <cstdint>
#include <string_view>

namespace alttech::modules::macro {

// When a daily observation became usable.
//
// This exists because `ingest_time` cannot answer the question for history.
// AltMacro loaded ten months of daily series in one afternoon, so every row
// carries an ingest stamp from that afternoon; selecting on it would make the
// entire backfill unavailable until today, and selecting on the observation
// DATE instead would hand a model figures that were still unpublished.
//
// So availability is modelled from each source's release schedule, and always
// on the conservative side -- later than the truth rather than earlier. A
// feature that becomes usable an hour late costs a little signal; one that
// becomes usable a day early manufactures it.
//
// The schedule that matters is the one of the path the number actually
// travels here, not of the institution that sets it. Every daily series in
// this block reaches AltMacro through an aggregator (FRED, Frankfurter), and an
// aggregator republishes on its own clock: the Treasury posts its curve the
// same evening, FRED carries it the next business day. Modelling the
// Treasury's clock made the first backfill of this block usable up to a day
// before AltMacro could have held the figure. The lags below were therefore
// set from what AltMacro's own ingest times showed once they were kept, with a
// margin, and each is written as a rule on the observation's weekday because
// the aggregators' release days are not uniform across the week:
//
//   CFETS / PBOC central parity, via Frankfurter
//       Set at 09:15 Beijing (01:15 UTC) on the observation day; Frankfurter
//       carried it by 06-07 UTC on the day measured. Usable from 08:00 UTC the
//       same day. Still the only same-day series in the block.
//
//   FRED daily: Treasury constant maturities (DGS2, DGS10, DFII10), Cboe VIX,
//   S&P 500, NASDAQ-100
//       FRED updates these the NEXT business day, the Treasury series after
//       the H.15 release at 16:15 New York (measured: 21:03 UTC the next day).
//       Usable from 00:00 UTC of the day after the next business day: a
//       Monday-to-Thursday observation waits two days, a Friday one waits
//       until Tuesday.
//
//   FRED weekly: Federal Reserve H.10 (DEXCHUS, DEXKOUS, DEXUSUK, DEXUSAL,
//   DEXJPUS, DEXUSEU)
//       One release per week, Monday afternoon New York, covering the week
//       just ended. Usable from 00:00 UTC of the Tuesday after that Monday: a
//       Monday observation waits eight days, a Friday one four. These are the
//       slowest series in the block by a wide margin, and they are slow
//       because that is what the source is.
//
// US holidays shift both FRED schedules by a business day and are not
// modelled; on such a week the modelled history is early by one day for the
// affected series. That is the residual look-ahead of this model, it is bounded,
// it is confined to backfilled history (a live computation can only use what
// AltMacro already holds), and AltMacro's preserved ingest_time is what lets
// an audit measure it.
//
// Returns the UTC instant from which the observation may be used.
[[nodiscard]] int64_t available_at_ms(std::string_view instrument_code,
                                      int64_t observation_start_ms) noexcept;

// The intraday series have a schedule of their own. Dukascopy publishes its
// tick archive as one file per completed HOUR, and AltMacro reads that file
// on its next pass, so no minute of an hour exists here before the hour has
// ended. Measured on the JetStream first-publish stamps of a full trading day
// (2026-09-11, USDCNH): the last bar of an hour arrived 8-26 minutes after
// the hour closed in normal operation; the exceptions were a service stall
// and the Friday close, both later. The rule takes thirty minutes.
//
// Returns the exclusive cutoff: every 1m bar whose open is BEFORE it is
// usable at `t_ms`; nothing at or after it is. It moves in hourly steps, so
// within one wall-clock hour every vector sees the same intraday snapshot --
// which is exactly what a live consumer of this feed would have seen.
[[nodiscard]] int64_t intraday_cutoff_ms(int64_t t_ms) noexcept;

// True when `observation_start_ms` is usable at `t0_ms`.
[[nodiscard]] inline bool available_at(std::string_view instrument_code,
                                       int64_t observation_start_ms, int64_t t0_ms) noexcept {
    return available_at_ms(instrument_code, observation_start_ms) <= t0_ms;
}

} // namespace alttech::modules::macro
