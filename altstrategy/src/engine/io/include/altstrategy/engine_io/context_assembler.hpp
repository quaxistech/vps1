#pragma once

// context_assembler (§7, §17, §40). Accumulates the latest AltTech analytics
// components, the AltSpotter universe status and the AltML forecast per pair,
// and — when a coherent revision is available — assembles an atomic, versioned,
// point-in-time DecisionContext. AltTech ships compact ORDERED feature vectors;
// the assembler decodes them to named features via a configured feature-set
// name list (docs/DECISION_CONTEXT.md), never stitching unrelated stale inputs.

#include "altstrategy/engine/decision_types.hpp"
#include "altstrategy/engine_io/wire_codec.hpp"

#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace altstrategy::engine_io {

// One AltTech vector this consumer reads, pinned to the exact producer set that
// defines its ordering.
//
// AltTech ships compact ORDERED vectors and states their identity on every
// message (`feature_set_name`, `feature_set_version`, `schema_hash`). Position
// is only meaningful relative to that identity, so the identity is pinned here
// and checked on every message: a producer that re-orders, adds or drops a
// member gets a new schema_hash and its vectors are refused instead of being
// silently misread. `talib-core` alone has 995 members, and reading position 9
// as "atr_14" when it is really a 20-bar rolling high is an error nothing
// downstream can detect.
//
// `positions` maps the name THIS engine uses to the producer's index. It is
// deliberately sparse: a consumer needs a handful of the 995, and listing only
// those keeps the mapping auditable. `value_count` is the producer's full
// vector length and is verified too, so a sparse map can never be applied to a
// vector it was not written for.
struct PositionalFeatureSet {
    std::string name;
    int32_t version = 0;
    std::string schema_hash;
    size_t value_count = 0;
    std::map<std::string, size_t> positions;

    [[nodiscard]] bool configured() const { return !positions.empty(); }
};

struct AssemblerConfig {
    // How old the last built context may be for a full-state snapshot (see
    // state_snapshot): one decision bar plus AltTech's availability tail.
    int64_t state_max_context_age_ms = 20LL * 60 * 1000;
    PositionalFeatureSet bitget_features;         // AltTech talib-core
    // Higher timeframes admitted as context, in minutes. Empty = execution grid
    // only. Grids the strategies do not ask for are not stored: the context is a
    // decision-ready abstraction, not a copy of the feature store.
    std::vector<int32_t> context_timeframes_minutes;
    // How stale a higher-timeframe observation may be, measured from its bar
    // CLOSE. 0 = fall back to max_component_age_ms.
    int64_t max_context_age_ms = 0;
    PositionalFeatureSet cross_venue_features;    // AltTech cross_venue_core
    PositionalFeatureSet microstructure_features; // AltTech bitget_micro
    // The ONE microstructure window admitted, e.g. "1m". AltTech publishes the
    // same 26 columns on 1s/5s/15s/30s/1m, and mixing them is not a freshness
    // question but a units question: trade_intensity, signed_volume and
    // cvd_delta are per-window quantities, so a 1s reading and a 1m reading of
    // the same market differ by a factor of sixty. Keeping "the newest" always
    // chose 1s -- measured, mean |cvd_delta| in built contexts was 407 against
    // 28 301 in the 1m windows -- and the book columns, which short windows
    // usually have no snapshot for, reached only 12% of contexts.
    // Empty admits any window, which is only correct for a single-window
    // producer.
    std::string microstructure_window = "1m";
    // The single timeframe decisions are made on, in minutes. AltTech publishes
    // a feature vector per timeframe on one subject; only this one is admitted,
    // so every indicator in a context describes the same bar length. 0 accepts
    // any, which is only ever correct for a single-timeframe producer.
    int32_t decision_timeframe_minutes = 15;
    int64_t context_ttl_ms = 5LL * 60 * 1000;
    int64_t max_component_age_ms = 20LL * 60 * 1000;   // reject stale components
    // A microstructure window is a live liquidity reading, not a bar analytic;
    // beyond this age the spread and depth it reports are not the market a plan
    // would be placed into, so the plan falls back to explicit defaults.
    int64_t max_microstructure_age_ms = 120'000;
    // How old an AltML econ estimate may be, from the close of its 1m bar,
    // to enter a context at all.
    int64_t max_forecast_age_ms = 180'000;
    double default_fees_bps = 4.0;
};

// Decode an ordered AltTech vector against a pinned producer set. The decode is
// refused (schema_mismatch, no values) unless the message's producer identity
// and vector length are the ones `set` was written for; slots the producer
// marked unusable in `validity_bitmap` are dropped rather than read.
[[nodiscard]] FeatureSummary decode_vector(const PositionalFeatureSet& set,
                                           const ParsedAltTech& message);

// Record one minute's open-interest change in a bar-ordered window of the last
// hour (see the definition for why a re-emitted bar replaces only itself).
void record_oi_change(std::deque<std::pair<int64_t, double>>& q, int64_t bar_open_ms,
                      double change);

// The session VWAP: volume-weighted average traded price since the start of the
// UTC day, accumulated from AltTech's closed 1m trade windows as
// sum(vwap_i * volume_i) / sum(volume_i) -- the traded notional over the traded
// volume. The UTC day is the session AltTech's own structure module anchors its
// `session_vwap` at, so the stack has one definition of "session".
//
// VWAP is a SESSION benchmark (Berkowitz, Logue & Noser 1988); a "VWAP
// reclaim" is a statement about that level. The context used to carry the VWAP
// of the last 1-minute window as `vwap`, i.e. the price of the minute just
// gone: price crossed it in one direction or the other on 89.9% of decision
// bars over seven days, so the reclaim test selected nothing.
//
// Point-in-time: only closed windows are recorded, in order, each once. The
// value is ABSENT, never partial, unless the window at the session's open was
// observed and at least kSessionVwapMinCoverage of the session's minutes were
// -- a VWAP accumulated from 09:00 is not the session VWAP, it is a different
// anchor that looks the same.
struct SessionVwap {
    static constexpr int64_t kSessionMs = 86'400'000;
    static constexpr int64_t kMinuteMs = 60'000;
    // The first window counted must start this close to the session open.
    static constexpr int64_t kAnchorToleranceMs = 5 * kMinuteMs;
    // Share of the session's minutes that must have been observed; the same
    // 75% the one-hour OI window asks of its minutes.
    static constexpr double kMinCoverage = 0.75;

    int64_t session_start_ms = -1;
    int64_t first_window_ms = 0;
    int64_t last_window_ms = 0;
    int64_t windows = 0;       // windows observed, traded or not
    double notional = 0;       // sum(vwap_i * volume_i)
    double volume = 0;         // sum(volume_i)

    // One closed 1m window. `vwap`/`window_volume` absent (or non-positive) is a
    // window with no trade: it counts toward coverage and adds nothing to the
    // sums. A window from an earlier session, or not newer than the last one
    // counted, is ignored -- never counted twice.
    void record(int64_t window_start_ms, std::optional<double> vwap,
                std::optional<double> window_volume);
    [[nodiscard]] std::optional<double> value() const;
};

// One closed 1m candle's traded volume, for rebuilding the session VWAP after a
// restart: quote / base is the minute's VWAP, base its volume -- the same
// definition the live 1m microstructure windows accumulate.
struct SessionMinute {
    int64_t open_ms = 0;
    double quote_volume = 0;
    double base_volume = 0;
};

class ContextAssembler {
public:
    explicit ContextAssembler(AssemblerConfig cfg) : cfg_(std::move(cfg)) {}

    void ingest_alttech(const ParsedAltTech& p);
    void ingest_forecast(const ParsedForecast& f);
    void set_universe(int64_t pair, const std::string& status);

    // Try to build a fresh context for `pair` as-of now_ms. Returns nullopt if
    // the components are incomplete, stale or misaligned. Monotonic revision.
    [[nodiscard]] std::optional<DecisionContext> try_build(int64_t pair, int64_t now_ms);

    // The market state as of now for full-state decisions, between decision
    // bars: the last built context with the estimates, price and funding that
    // have arrived since. AltML re-estimates every closed 1m bar; the 15m
    // context only frames it. nullopt when no context was built recently.
    [[nodiscard]] std::optional<DecisionContext> state_snapshot(int64_t pair, int64_t now_ms) const;

    [[nodiscard]] size_t tracked_pairs() const;

    // True while a build for `pair` is held back waiting for the next
    // microstructure window (see try_build); the engine retries the build when
    // that window arrives.
    [[nodiscard]] bool awaiting_microstructure(int64_t pair) const;
    // True while a build is held back waiting for the information venues'
    // vectors of the decision bar (see try_build).
    [[nodiscard]] bool awaiting_information_venues(int64_t pair) const;

    // How long a build waits for the information venues' vectors of the same
    // decision bar. Measured on VPS1 over six hours of 15m bars: they are
    // computed a median 12-16 s after Bitget's and 30-41 s at the 90th
    // percentile; 45 s covers that, and a venue later than that is absent.
    static constexpr int64_t kReferenceWaitMs = 45'000;

    // The session VWAP after a restart. The live accumulation needs the
    // session's windows from its first minute, so a restart used to leave it
    // ABSENT until the next UTC midnight -- and with it every setup that reads
    // it. The engine asks this for the first pinned window of a session the
    // pair has not accumulated yet; the answer is the session start to seed
    // from (nullopt: nothing to seed).
    [[nodiscard]] std::optional<int64_t> session_vwap_seed_needed(int64_t pair,
                                                                  int64_t window_start_ms,
                                                                  const std::string& window_label) const;
    // Record [session start, first live window) from closed 1m candles.
    void seed_session_vwap(int64_t pair, int64_t session_start_ms,
                           const std::vector<SessionMinute>& minutes);

    // How long a build may wait for the next microstructure window: the
    // producer's publication jitter with a wide margin (measured p50 60.5 s,
    // p99 61.4 s after window close).
    static constexpr int64_t kMicroWaitGraceMs = 15'000;

private:
    // Pair+grid+reason keys already reported, so a persistently rejected context
    // grid is logged once rather than on every decision.
    std::set<std::string> grid_skip_logged_;

    struct PairState {
        std::optional<DecisionContext> last_built;           // for state_snapshot
        FeatureSummary bitget_features;                      // execution grid
        std::map<int32_t, FeatureSummary> context_features;  // higher grids, by minutes
        // Per information venue, keyed by AltTech exchange id.
        std::map<int32_t, FeatureSummary> reference_features;
        FeatureSummary cross_venue_features;
        FeatureSummary micro_features;   // live spread / depth / flow
        RegimeView bitget_regime;
        RegimeView global_regime;
        std::vector<ProjectedZone> zones;
        MarketForecastView forecast;
        std::map<int32_t, MarketForecastView> forecasts;   // econ, by horizon
        std::map<int32_t, RegimeView> htf_regimes;         // Bitget 15m / 1h
        int64_t micro_wait_since_ms = 0;                   // see try_build
        int64_t ref_wait_bar_ms = 0;                       // decision bar being waited for
        int64_t ref_wait_since_ms = 0;
        bool ref_waiting = false;
        int64_t seeded_session_ms = -1;                    // session VWAP seeded for
        ContextQuality quality = ContextQuality::strict_4_of_4;
        uint32_t venue_mask = 0, missing_venue_mask = 0;
        bool regime_divergence = false;
        std::string universe_status = "ELIGIBLE_ALL_VENUES";
        std::string feature_manifest_hash;
        int64_t last_component_ms = 0;
        int64_t last_emitted_revision = -1;
        // Last OBSERVED book spread and depth, and when they were observed.
        //
        // Held separately from `micro_features` because a microstructure window
        // is an average over its bucket, and the book is sampled every 5 s
        // (core) or 15 s (other) against 15 s buckets -- so a bucket routinely
        // contains no sample at all and averages to zero. Reading the spread
        // only from the freshest window therefore discarded most real readings:
        // measured here at 0-6 of every 18 windows per instrument.
        //
        // A spread observed twelve seconds ago is a valid observation as of T0;
        // an empty bucket is not evidence that the spread is unknown. Point-in-
        // time is preserved because these are only ever written from windows
        // that have already closed, so nothing later than T0 can enter.
        double last_spread_bps = 0;
        double last_top_depth_usdt = 0;
        int64_t last_book_obs_ms = 0;        // window CLOSE of that reading
        int64_t last_book_obs_start_ms = 0;  // window start, for ordering only

        // Where price sat relative to VWAP on the last CLOSED execution bar,
        // and the bar at which that side last changed.
        //
        // A "reclaim" is by definition a statement about the recent past: price
        // was on the other side of VWAP and has come back. A point-in-time
        // context carries one bar, so the strategy that tested for it read a
        // feature name -- `vwap_lost_recently` -- that no producer publishes,
        // got the 0 default on every evaluation, and emitted zero candidates
        // for its entire deployed life. This is the crossing observed here, and
        // only from bars that have already closed, so nothing later than T0
        // enters the decision.
        // Minute-by-minute consensus OI changes, kept over the crowding window.
        //
        // AltTech publishes `consensus_oi_change` = (OI[t] - OI[t-1min]) /
        // OI[t-1min]: an instantaneous delta, and nothing anywhere publishes an
        // accumulated one. funding_oi_crowding_unwind asks for CROWDING -- OI
        // built up over hours -- and tested the delta against 0.08. Measured:
        // the delta's median is 0.000, p95 0.0013 and its 7-day maximum 0.0616,
        // so the gate could not open once. Over an hour the same market gives a
        // median of 0.0026, p95 0.0169 and crosses 0.08 four times in three days
        // -- rare, which is what "extreme" is supposed to mean. The threshold is
        // unchanged; the input is now the quantity the hypothesis names.
        std::deque<std::pair<int64_t, double>> oi_changes;  // (bar_open_ms, ratio)

        int vwap_side = 0;              // +1 above, -1 below, 0 unknown
        int64_t vwap_flip_bar_ms = 0;   // open time of the bar the side changed
        int64_t vwap_side_bar_ms = 0;   // open time the side above was read at

        // The same side history against the SESSION VWAP (see SessionVwap).
        SessionVwap session_vwap;
        int session_vwap_side = 0;
        int64_t session_vwap_flip_bar_ms = 0;
        int64_t session_vwap_side_bar_ms = 0;

        // The Donchian channel as it stood on the PREVIOUS execution bar.
        //
        // AltTech's donchian_high_20 is TA-Lib MAX(high, 20), which includes the
        // bar that just closed. A breakout is price leaving the range the
        // previous N bars made (Brock, Lakonishok & LeBaron 1992, "trading range
        // break"); measured against a channel that already contains the
        // breakout bar's own high, price has to exceed that high AFTER the bar
        // closed. Over seven days that happened on 270 decision bars, against
        // 619 breaks of the prior channel.
        int64_t prev_exec_bar_ms = 0;
        double prev_donchian_high = 0;
        double prev_donchian_low = 0;
    };

    AssemblerConfig cfg_;
    mutable std::mutex mu_;
    std::map<int64_t, PairState> pairs_;
};

} // namespace altstrategy::engine_io
