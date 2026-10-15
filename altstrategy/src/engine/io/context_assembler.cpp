#include "altstrategy/engine_io/context_assembler.hpp"

#include <map>

#include "altstrategy/engine/ids.hpp"
#include "altstrategy/logging/logger.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <set>
#include <utility>

namespace altstrategy::engine_io {

namespace {

// Used only when no recent order-book reading is available. Deliberately
// pessimistic: a plan built on an assumed-tight market that is actually wide is
// the expensive mistake, the reverse merely declines a trade.
constexpr double kFallbackSpreadBps = 10.0;
constexpr double kFallbackTopDepthUsdt = 5'000.0;
// Impact charged to the widest book on top of half the spread.
constexpr double kDepthImpactBps = 5.0;
// The liquidity ordinal runs 1..engine::kVenueUniverseSize (decision_types.hpp).
using engine::kVenueUniverseSize;
// Floor for the log volume ratio: below this the window is effectively empty and
// the exact magnitude carries no signal. e^-5 is about 0.7 % of the previous
// window's volume.
constexpr double kMinVolumeSurge = -5.0;

// |ROC(5)| below this (percent, as TA-Lib reports ROC) counts as "price is not
// going anywhere" for the crowding strategies.
constexpr double kStallRocPct = 0.10;

// Composites the engine reasons about that NO producer publishes. Each is
// derived here from pinned raw members, with the formula stated, rather than
// being read from a feature name that does not exist — which is what silently
// yielded zeros before. Nothing is invented: if an input is missing the
// composite is simply absent and the strategy's own default applies.
void derive_engine_features(FeatureSummary& f, const FeatureSummary& micro, double price) {
    const auto have = [&](const char* k) { return f.has(k); };

    // Volatility expansion: current ATR against the slow ATR. 1.0 = unchanged,
    // >1 = expanding. Dimensionless, so it is comparable across instruments.
    if (have("atr_14") && have("atr_50") && f.get("atr_50") > 0) {
        f.values["atr_expansion"] = f.get("atr_14") / f.get("atr_50");
    }
    // Trend compression: how close the fast and slow EMA are, as a fraction of
    // price, so the threshold does not depend on the asset's price.
    if (have("ema_20") && have("ema_50") && price > 0) {
        f.values["ema_compression"] = std::abs(f.get("ema_20") - f.get("ema_50")) / price;
    }
    // Bollinger bandwidth, the standard (upper - lower) / middle.
    if (have("bb_upper_20") && have("bb_lower_20") && f.get("bb_middle_20") > 0) {
        f.values["bb_width_20"] =
            (f.get("bb_upper_20") - f.get("bb_lower_20")) / f.get("bb_middle_20");
    }
    // Momentum horizons, named for what they actually are. TA-Lib's ROC is a
    // percent change over N bars; the engine used to ask for "mom_1m", a
    // one-minute horizon nothing publishes.
    if (have("roc_5")) f.values["mom_short"] = f.get("roc_5");
    if (have("roc_14")) f.values["mom_mid"] = f.get("roc_14");
    if (have("roc_20")) f.values["mom_long"] = f.get("roc_20");
    // Stalling price: short-horizon momentum flat. Used together with rising
    // open interest to detect crowding.
    if (have("roc_5")) {
        f.values["price_stall"] = std::abs(f.get("roc_5")) < kStallRocPct ? 1.0 : 0.0;
    }
    // Deviation from the session VWAP, in BASIS POINTS. VWAP is a traded-flow
    // quantity and comes from the microstructure window.
    //
    // The unit is not cosmetic. This was produced as a bare fraction while its
    // only consumer compares it against `reclaim_hold_bps` (default 3) and
    // scales it by 10 -- reading a fraction there asks for a 30000 bps move, so
    // the test could not pass at any price. Basis points is the unit the
    // consumer, the parameter name and the parameter bounds all already assume.
    if (micro.has("vwap") && micro.get("vwap") > 0 && price > 0) {
        f.values["vwap"] = micro.get("vwap");
        f.values["vwap_dev"] = (price - micro.get("vwap")) / price * 10'000.0;
    }
}

// How many closed bars after a VWAP crossing still count as a "reclaim".
// Five bars is the span over which the crossing is still the dominant recent
// event on the execution grid; beyond that the move is simply a trend.
constexpr int64_t kVwapReclaimBars = 5;

// Crowding window for accumulated open interest, and how much of it must
// actually be observed. One hour of minute deltas is 60 samples; requiring 45
// keeps a short gap from silently shortening the window, while a longer gap
// leaves the feature ABSENT rather than understated.
constexpr int64_t kOiCrowdingWindowMs = 60LL * 60 * 1000;
constexpr size_t kOiCrowdingMinSamples = 45;

} // namespace

// One minute's OI change, kept in bar order.
//
// A re-emitted bar (AltTech republishes a minute when a late venue arrives)
// replaces its own entry and nothing else. The first version popped from the
// back while the back was at or after the incoming bar, so a repeat of a bar
// from twenty minutes ago discarded the twenty newer minutes with it; and it
// trimmed the window against the INCOMING bar, so an old repeat also dropped
// entries that were still inside the hour. Either way the deque fell short of
// the 45 samples oi_growth_1h requires.
void record_oi_change(std::deque<std::pair<int64_t, double>>& q, int64_t bar_open_ms,
                      double change) {
    const auto it = std::lower_bound(
        q.begin(), q.end(), bar_open_ms,
        [](const std::pair<int64_t, double>& e, int64_t bar) { return e.first < bar; });
    if (it != q.end() && it->first == bar_open_ms) {
        it->second = change;
    } else {
        q.insert(it, {bar_open_ms, change});
    }
    const int64_t cutoff = q.back().first - kOiCrowdingWindowMs;
    while (!q.empty() && q.front().first <= cutoff) q.pop_front();
}

void SessionVwap::record(int64_t window_start_ms, std::optional<double> vwap,
                         std::optional<double> window_volume) {
    if (window_start_ms < 0) return;
    const int64_t session = window_start_ms - window_start_ms % kSessionMs;
    if (session < session_start_ms) return;              // a window of a past session
    if (session > session_start_ms) {
        *this = SessionVwap{};
        session_start_ms = session;
        first_window_ms = window_start_ms;
    } else if (window_start_ms <= last_window_ms) {
        return;                                          // already counted
    }
    last_window_ms = window_start_ms;
    ++windows;
    if (vwap && window_volume && std::isfinite(*vwap) && std::isfinite(*window_volume) &&
        *vwap > 0.0 && *window_volume > 0.0) {
        notional += *vwap * *window_volume;
        volume += *window_volume;
    }
}

std::optional<double> SessionVwap::value() const {
    if (session_start_ms < 0 || !(volume > 0.0)) return std::nullopt;
    if (first_window_ms - session_start_ms > kAnchorToleranceMs) return std::nullopt;
    const int64_t elapsed = (last_window_ms - session_start_ms) / kMinuteMs + 1;
    if (static_cast<double>(windows) < kMinCoverage * static_cast<double>(elapsed))
        return std::nullopt;
    return notional / volume;
}

namespace {

// Length of a microstructure window label, in ms. A window's information does
// not exist until the window CLOSES, so its age is measured from the close --
// the same rule the higher grids use. Measuring from the window START instead
// makes a 1m window 60 s old the instant it becomes available, and AltTech
// publishes it 121 s after that start: against a 120 s bound every window was
// discarded on arrival.
[[nodiscard]] int64_t window_span_ms(std::string_view label) {
    if (label == "1s") return 1'000;
    if (label == "5s") return 5'000;
    if (label == "15s") return 15'000;
    if (label == "30s") return 30'000;
    if (label == "1m") return 60'000;
    return 0;   // unknown label: no span assumed
}

bool validity_bit(std::string_view bitmap, size_t index) {
    // Producer convention: bit i lives in byte i/8 at offset i%8, counted from
    // the least significant end. An absent/short bitmap means "not reported",
    // which is treated as usable so a producer that omits it is not silently
    // blanked; a present bitmap is authoritative.
    const size_t byte = index / 8;
    if (byte >= bitmap.size()) return true;
    return ((static_cast<unsigned char>(bitmap[byte]) >> (index % 8)) & 1U) != 0U;
}

// A schema mismatch is a deployment fault, not a data event: it does not heal
// on the next message and it would otherwise repeat for every bar of every
// pair. Report each distinct (kind, producer length) once and stay quiet after.
void report_schema_mismatch(const char* kind, const PositionalFeatureSet& set,
                            const ParsedAltTech& message) {
    static std::mutex seen_mu;
    static std::set<std::pair<std::string, std::string>> seen;
    {
        std::lock_guard<std::mutex> lk(seen_mu);
        if (!seen.emplace(kind, message.schema_hash).second) return;
    }
    log::critical(
        "alttech_feature_schema_mismatch",
        {{"vector", kind},
         {"expected_set", set.name},
         {"expected_version", set.version},
         {"expected_schema_hash", set.schema_hash},
         {"expected_value_count", static_cast<int64_t>(set.value_count)},
         {"producer_set", message.feature_set_name},
         {"producer_version", message.feature_set_version},
         {"producer_schema_hash", message.schema_hash},
         {"producer_value_count", static_cast<int64_t>(message.values.size())},
         {"effect",
          "vector refused; no decision context is built from it. Position is only "
          "meaningful relative to the producer set that defines the ordering, so a "
          "positional decode against a different set would bind values to the wrong "
          "names. Re-pin the set in the context config from the producer's catalog."}});
}

} // namespace

FeatureSummary decode_vector(const PositionalFeatureSet& set, const ParsedAltTech& message) {
    FeatureSummary f;
    f.bar_open_time_ms = message.bar_open_time_ms;
    f.revision = message.revision;
    f.producer_value_count = message.values.size();

    // The producer states which set it is sending. If that is not the set these
    // positions were written against, the positions mean nothing and binding
    // them would attach real numbers to the wrong names.
    const bool identity_ok =
        (set.name.empty() || set.name == message.feature_set_name) &&
        (set.version == 0 || set.version == message.feature_set_version) &&
        (set.schema_hash.empty() || set.schema_hash == message.schema_hash) &&
        (set.value_count == 0 || set.value_count == message.values.size());
    if (!set.configured() || !identity_ok) {
        f.valid = false;
        f.schema_mismatch = true;
        return f;
    }
    f.valid = message.features.valid;

    for (const auto& [engine_name, index] : set.positions) {
        if (index >= message.values.size()) continue;             // pinned above; belt and braces
        if (!validity_bit(message.validity_bitmap, index)) continue;  // producer says unusable
        const double v = static_cast<double>(message.values[index]);
        if (std::isfinite(v)) f.values[engine_name] = v;
    }
    return f;
}

void ContextAssembler::ingest_alttech(const ParsedAltTech& p) {
    if (p.kind == AltTechKind::none || p.canonical_pair_id == 0) return;
    std::lock_guard<std::mutex> lk(mu_);
    auto& st = pairs_[p.canonical_pair_id];
    st.last_component_ms = std::max(st.last_component_ms, p.bar_open_time_ms);
    switch (p.kind) {
        case AltTechKind::bitget_features: {
            // AltTech publishes a vector per timeframe (1m … 1d) on this tag.
            // Accepting all of them made the context hold whichever arrived
            // last, so a stop could be sized from a daily ATR on one bar and a
            // five-minute ATR on the next. Decisions run on ONE timeframe.
            const bool is_execution_tf = cfg_.decision_timeframe_minutes <= 0 ||
                                         p.timeframe_minutes == cfg_.decision_timeframe_minutes;
            const bool is_context_tf =
                !is_execution_tf &&
                std::find(cfg_.context_timeframes_minutes.begin(),
                          cfg_.context_timeframes_minutes.end(),
                          p.timeframe_minutes) != cfg_.context_timeframes_minutes.end();
            // A grid that is neither the execution one nor a declared context
            // one is dropped -- deliberately, so the context stays a
            // decision-ready abstraction rather than the whole feature store.
            if (!is_execution_tf && !is_context_tf) break;

            auto decoded = decode_vector(cfg_.bitget_features, p);
            if (decoded.schema_mismatch) {
                report_schema_mismatch("bitget_features", cfg_.bitget_features, p);
                // Keep the last good summary rather than replacing it with an
                // empty one: try_build() gates on validity, and an unreadable
                // message must not look like fresh data.
                break;
            }
            if (is_context_tf) {
                // Higher-timeframe context keeps its own slot, keyed by bar
                // length. Overwriting the execution summary with it is what made
                // the context hold whichever grid arrived last.
                //
                // Logged the first time each grid is seen for each pair: the
                // multi-timeframe context is invisible from outside until a
                // decision carries it, so "no grids in any context" could mean
                // the vectors never arrived, arrived invalid, or arrived and
                // were rejected at admission. This says which.
                const bool first_time = !st.context_features.count(p.timeframe_minutes);
                auto& grid = st.context_features[p.timeframe_minutes];
                // Bar-ordered like the execution grid below: a backfilled bar
                // (admissible for up to max_context_age) never replaces a newer one.
                if (!first_time && decoded.bar_open_time_ms < grid.bar_open_time_ms) break;
                grid = std::move(decoded);
                if (first_time) {
                    const auto& g = st.context_features[p.timeframe_minutes];
                    log::info("context_grid_received",
                              {{"pair", p.canonical_pair_id},
                               {"timeframe_minutes", p.timeframe_minutes},
                               {"valid", g.valid},
                               {"values", static_cast<int64_t>(g.values.size())},
                               {"bar_open_time_ms", g.bar_open_time_ms}});
                }
                break;
            }
            // Never regress to an older execution bar: AltTech's feature engine
            // re-publishes past bars when it backfills.
            if (decoded.bar_open_time_ms < st.bitget_features.bar_open_time_ms) break;
            // A NEW bar moves the one held into the previous-bar slot; a
            // re-publication of the same bar (a later revision) does not.
            if (decoded.bar_open_time_ms > st.bitget_features.bar_open_time_ms &&
                st.bitget_features.valid && st.bitget_features.has("donchian_high_20") &&
                st.bitget_features.has("donchian_low_20")) {
                st.prev_exec_bar_ms = st.bitget_features.bar_open_time_ms;
                st.prev_donchian_high = st.bitget_features.get("donchian_high_20");
                st.prev_donchian_low = st.bitget_features.get("donchian_low_20");
            }
            st.bitget_features = std::move(decoded);
            st.feature_manifest_hash = p.schema_hash;
            break;
        }
        case AltTechKind::reference_features: {
            // An information venue's vector. Same decision grid as the target --
            // a 1h Binance reading is context for a 1h decision, not for a 1m
            // one -- and the same schema check, because a positional decode is
            // only sound against the layout that produced it.
            if (cfg_.decision_timeframe_minutes > 0 &&
                p.timeframe_minutes != cfg_.decision_timeframe_minutes) {
                break;
            }
            if (p.venue < 2 || p.venue > 4) break;   // target venue is not a reference
            auto decoded = decode_vector(cfg_.bitget_features, p);
            if (decoded.schema_mismatch) {
                // Not silent: the reference set has its own layout, and a vector
                // whose schema this consumer cannot read is reported and dropped
                // rather than decoded against someone else's ordering.
                report_schema_mismatch("reference_features", cfg_.bitget_features, p);
                break;
            }
            // Bar-ordered, as the target's own vector is.
            auto& reference = st.reference_features[p.venue];
            if (decoded.bar_open_time_ms < reference.bar_open_time_ms) break;
            reference = std::move(decoded);
            break;
        }
        case AltTechKind::cross_venue_features: {
            auto decoded = decode_vector(cfg_.cross_venue_features, p);
            if (decoded.schema_mismatch) {
                report_schema_mismatch("cross_venue_features", cfg_.cross_venue_features, p);
                break;
            }
            // Accumulate the OI deltas before the summary is overwritten. Each
            // vector carries one minute's change; crowding is what they compound
            // to. Only bars that have already closed are appended, and a repeat
            // of a bar already held replaces it rather than compounding twice --
            // AltTech re-emits a bar when a late venue arrives.
            if (decoded.has("oi_growth") && decoded.bar_open_time_ms > 0) {
                const double r = decoded.get("oi_growth");
                if (std::isfinite(r) && r > -1.0) {
                    record_oi_change(st.oi_changes, decoded.bar_open_time_ms, r);
                }
            }
            // The summary is the NEWEST bar's, never an older one's. AltTech's
            // sweep computes the newest uncomputed minute first and fills the
            // gaps behind it on later passes, publishing each; keeping the last
            // to ARRIVE let a backfilled minute from up to lookback_min ago
            // replace the current one -- and consensus_mid_price, the decision
            // price, is read from this summary. A re-emit of the held bar (a
            // late venue) still replaces it.
            if (decoded.bar_open_time_ms >= st.cross_venue_features.bar_open_time_ms) {
                st.cross_venue_features = std::move(decoded);
                st.quality = p.quality; st.venue_mask = p.venue_mask;
                st.missing_venue_mask = p.missing_venue_mask;
            }
            break;
        }
        case AltTechKind::bitget_microstructure: {
            // One window only. See AssemblerConfig::microstructure_window: the
            // five window lengths carry the same column names in different
            // units, so admitting all of them and keeping the newest is a unit
            // error that looks like freshness.
            if (!cfg_.microstructure_window.empty() &&
                p.window_label != cfg_.microstructure_window) {
                break;
            }
            auto decoded = decode_vector(cfg_.microstructure_features, p);
            if (decoded.schema_mismatch) {
                report_schema_mismatch("microstructure_features", cfg_.microstructure_features, p);
                break;
            }
            // Record any ACTUAL book reading this window carried, regardless of
            // whether the window itself is the freshest one. See PairState:
            // most windows carry none, and dropping the ones that do is how the
            // real spread stayed invisible even after the book started flowing.
            const double obs_spread = decoded.get("spread_bps", 0.0);
            if (obs_spread > 0.0 && std::isfinite(obs_spread) &&
                decoded.bar_open_time_ms >= st.last_book_obs_start_ms) {
                st.last_spread_bps = obs_spread;
                st.last_book_obs_start_ms = decoded.bar_open_time_ms;
                // Stored as the window's CLOSE, because that is when the reading
                // EXISTS -- the same rule the bars and the microstructure block
                // use. Held as the start, a 1m window was already 60 s old when
                // it was published (AltTech publishes it ~121 s after the window
                // starts), so against the 120 s freshness bound the book never
                // counted as observed: measured here, spread_bps was the
                // conservative 10.0 fallback in 351 of 351 contexts while the
                // producer was reporting a median of 1.59 bps.
                st.last_book_obs_ms =
                    decoded.bar_open_time_ms + window_span_ms(cfg_.microstructure_window);
                const double b = decoded.get("book_bid_depth", 0.0);
                const double a = decoded.get("book_ask_depth", 0.0);
                st.last_top_depth_usdt = (b + a > 0) ? b + a : 0.0;
            }
            // The session VWAP is accumulated from every pinned window, traded
            // or not; the window's volume is its aggressive buy + sell volume,
            // the denominator AltTech's own window VWAP is computed over.
            {
                const auto opt = [&](const char* k) -> std::optional<double> {
                    return decoded.has(k) ? std::optional<double>(decoded.get(k)) : std::nullopt;
                };
                std::optional<double> vol;
                if (const auto b = opt("aggressive_buy_volume"), a = opt("aggressive_sell_volume"); b && a)
                    vol = *b + *a;
                st.session_vwap.record(decoded.bar_open_time_ms, opt("vwap"), vol);
            }
            // Windows arrive out of order across labels (1s..1m); keep the
            // freshest reading, never let an older window overwrite a newer one.
            if (decoded.bar_open_time_ms >= st.micro_features.bar_open_time_ms) {
                st.micro_features = std::move(decoded);
            }
            break;
        }
        case AltTechKind::global_regime:
            // Published per cross-venue minute, so backfilled minutes arrive
            // after newer ones; an older reading never replaces a newer one.
            if (p.regime.bar_open_time_ms < st.global_regime.bar_open_time_ms) break;
            st.global_regime = p.regime; st.regime_divergence = p.regime_divergence;
            if (p.quality != ContextQuality::strict_4_of_4 || st.quality == ContextQuality::strict_4_of_4)
                st.quality = p.quality;
            break;
        case AltTechKind::bitget_regime:
            if (p.regime.bar_open_time_ms < st.bitget_regime.bar_open_time_ms) break;
            st.bitget_regime = p.regime;
            break;
        case AltTechKind::bitget_htf_regime:
            // Per timeframe, bar-ordered: a late message for an older bar never
            // replaces a newer classification.
            for (const auto& [tf, view] : p.htf_regimes) {
                auto& cur = st.htf_regimes[tf];
                if (view.bar_open_time_ms >= cur.bar_open_time_ms) cur = view;
            }
            break;
        case AltTechKind::projected_zone: {
            // Keep a bounded set of the most recent zones.
            st.zones.push_back(p.zone);
            if (st.zones.size() > 12) st.zones.erase(st.zones.begin());
            break;
        }
        case AltTechKind::cross_venue_quality:
            st.quality = p.quality; st.venue_mask = p.venue_mask; st.missing_venue_mask = p.missing_venue_mask;
            break;
        case AltTechKind::universe_change:
            if (!p.universe_status.empty()) st.universe_status = p.universe_status;
            break;
        default: break;
    }
}

void ContextAssembler::ingest_forecast(const ParsedForecast& f) {
    if (f.canonical_pair_id == 0) return;
    std::lock_guard<std::mutex> lk(mu_);
    auto& st = pairs_[f.canonical_pair_id];
    st.forecast = f.view;
    // One estimate per horizon, newest bar wins; a revision of the same bar
    // replaces it only if it is not older.
    if (f.view.econ && f.view.horizon_minutes > 0) {
        auto& cur = st.forecasts[f.view.horizon_minutes];
        if (!cur.present || f.view.bar_open_time_ms > cur.bar_open_time_ms ||
            (f.view.bar_open_time_ms == cur.bar_open_time_ms &&
             f.view.bar_revision >= cur.bar_revision)) {
            cur = f.view;
        }
    }
}

bool ContextAssembler::awaiting_microstructure(int64_t pair) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = pairs_.find(pair);
    return it != pairs_.end() && it->second.micro_wait_since_ms > 0;
}

bool ContextAssembler::awaiting_information_venues(int64_t pair) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = pairs_.find(pair);
    return it != pairs_.end() && it->second.ref_waiting;
}

std::optional<int64_t> ContextAssembler::session_vwap_seed_needed(int64_t pair,
                                                                  int64_t window_start_ms,
                                                                  const std::string& window_label) const {
    if (window_start_ms <= 0) return std::nullopt;
    if (!cfg_.microstructure_window.empty() && window_label != cfg_.microstructure_window) return std::nullopt;
    const int64_t session = window_start_ms - window_start_ms % SessionVwap::kSessionMs;
    if (window_start_ms == session) return std::nullopt;   // the session's own first window: live covers it
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = pairs_.find(pair);
    if (it != pairs_.end()) {
        if (it->second.session_vwap.session_start_ms == session) return std::nullopt;  // accumulating
        if (it->second.seeded_session_ms == session) return std::nullopt;             // tried once
    }
    return session;
}

void ContextAssembler::seed_session_vwap(int64_t pair, int64_t session_start_ms,
                                         const std::vector<SessionMinute>& minutes) {
    std::lock_guard<std::mutex> lk(mu_);
    auto& st = pairs_[pair];
    st.seeded_session_ms = session_start_ms;
    for (const auto& m : minutes) {
        if (m.open_ms < session_start_ms || m.open_ms >= session_start_ms + SessionVwap::kSessionMs) continue;
        const bool traded = m.base_volume > 0.0 && m.quote_volume > 0.0;
        st.session_vwap.record(m.open_ms,
                               traded ? std::optional<double>(m.quote_volume / m.base_volume) : std::nullopt,
                               traded ? std::optional<double>(m.base_volume) : std::nullopt);
    }
}

void ContextAssembler::set_universe(int64_t pair, const std::string& status) {
    if (pair == 0) return;
    std::lock_guard<std::mutex> lk(mu_);
    pairs_[pair].universe_status = status;
}

size_t ContextAssembler::tracked_pairs() const {
    std::lock_guard<std::mutex> lk(mu_);
    return pairs_.size();
}

std::optional<DecisionContext> ContextAssembler::try_build(int64_t pair, int64_t now_ms) {
    // Naming the refusal. This function declines for six different reasons and
    // used to return a bare nullopt for all of them, so a pipeline that stopped
    // producing contexts looked identical to one with nothing new to say -- and
    // the whole decision path simply went quiet with every upstream healthy.
    // Throttled to one line per pair per minute: contexts are attempted on
    // every inbound feature vector.
    static std::mutex why_mu;
    static std::map<int64_t, int64_t> last_why_ms;
    const auto decline = [&](const char* reason) -> std::optional<DecisionContext> {
        std::lock_guard<std::mutex> lk(why_mu);
        auto& last = last_why_ms[pair];
        if (now_ms - last >= 60'000) {
            last = now_ms;
            log::warn("context_not_built", {{"pair", pair}, {"reason", reason}});
        }
        return std::nullopt;
    };

    std::lock_guard<std::mutex> lk(mu_);
    auto it = pairs_.find(pair);
    if (it == pairs_.end()) return decline("no state for this pair yet");
    PairState& st = it->second;

    // Require the core components: Bitget features + a regime signal.
    // AltTech's cross-venue consensus regime (global_market_regime) is a
    // planned message that is not published by every AltTech deployment yet;
    // fall back to the single-venue Bitget regime classification (which IS
    // published) so the pipeline degrades gracefully instead of deadlocking
    // forever on an input that may never arrive. The true global regime is
    // preferred the moment it is present.
    if (!st.bitget_features.valid || st.bitget_features.values.empty())
        return decline("no valid bitget feature vector");
    const bool have_global = !st.global_regime.regime.empty() && st.global_regime.regime != "UNDEFINED";
    const bool have_bitget_regime = !st.bitget_regime.regime.empty() && st.bitget_regime.regime != "UNDEFINED";
    if (!have_global && !have_bitget_regime) return decline("no usable regime");
    const RegimeView& effective_regime = have_global ? st.global_regime : st.bitget_regime;
    // Freshness, measured from when the bar's information EXISTS -- its close --
    // not from when it opened.
    //
    // This compared `now - bar_open_time` against the bound. A bar on the
    // decision grid is only observable at bar_open + span, so age-from-open is
    // never less than the grid length: on a 15m grid a 20-minute bound left a
    // usable window of a few minutes per cycle and declined the rest. Measured
    // here: 2 294 declines for this reason in two hours, against 0 schema
    // mismatches and a healthy feed. The bound is meant to say "how stale may an
    // observation be", which is a statement about the close.
    const int64_t decision_span_ms =
        cfg_.decision_timeframe_minutes > 0
            ? static_cast<int64_t>(cfg_.decision_timeframe_minutes) * 60'000
            : 0;
    const int64_t bar_close_ms = st.bitget_features.bar_open_time_ms + decision_span_ms;
    if (now_ms - bar_close_ms > cfg_.max_component_age_ms)
        return decline("bitget features older than max_component_age");

    // Monotonic revision: derive from the freshest component bar time.
    const int64_t revision = std::max({st.bitget_features.revision, st.cross_venue_features.revision,
                                       st.bitget_features.bar_open_time_ms});
    if (revision <= st.last_emitted_revision)
        return decline("revision not newer than the last emitted");

    // The microstructure freshness race, resolved by WAITING, never by
    // widening the bound.
    //
    // AltTech computes a 1m window about 61 s after it closes (measured on
    // VPS1: p50 60.5 s, p99 61.4 s), so the newest window a consumer can hold
    // is always between 61 and ~121 s old -- and the bound is 120 s. A context
    // assembled in the last second before the next window lands found its
    // newest window 120.x s old and was built WITHOUT a book, although a fully
    // valid window was about to arrive. Relaxing the bound to 125 s would admit
    // readings the bound exists to refuse. Instead: when the held window has
    // just crossed the bound by less than the producer's own jitter, the build
    // waits for the next window (the engine retries the moment it arrives);
    // if none arrives within the grace, the context is built without one --
    // microstructure absent, never stale.
    {
        const int64_t span = window_span_ms(cfg_.microstructure_window);
        const int64_t close_ms = st.micro_features.bar_open_time_ms + span;
        const int64_t age = now_ms - close_ms;
        const bool held = st.micro_features.valid && st.micro_features.bar_open_time_ms > 0;
        const bool just_expired = held && age > cfg_.max_microstructure_age_ms &&
                                  age <= cfg_.max_microstructure_age_ms + kMicroWaitGraceMs;
        if (just_expired) {
            if (st.micro_wait_since_ms == 0) st.micro_wait_since_ms = now_ms;
            if (now_ms - st.micro_wait_since_ms < kMicroWaitGraceMs) {
                return decline("awaiting the next microstructure window");
            }
        }
        st.micro_wait_since_ms = 0;
    }

    // The information venues' vectors of the SAME decision bar.
    //
    // A context is attempted the moment Bitget's decision-bar vector arrives,
    // and the other venues' vectors of that bar are computed a median 12-16 s
    // later (p90 30-41 s). Built at once, the context held the venues'
    // PREVIOUS bar, so everything asked of the venues on the decision grid --
    // the 15m agreement -- was absent from 215 of 216 contexts. The build now
    // waits up to kReferenceWaitMs for two of them; the engine retries as they
    // arrive. Only for a pair whose venues have been seen at all: a pair with
    // no information venues has nothing to wait for.
    if (!st.reference_features.empty()) {
        const int64_t bar = st.bitget_features.bar_open_time_ms;
        int same_bar = 0;
        for (const auto& [venue, ref] : st.reference_features) {
            if (ref.valid && ref.bar_open_time_ms == bar) ++same_bar;
        }
        if (same_bar < 2) {
            if (st.ref_wait_bar_ms != bar) {
                st.ref_wait_bar_ms = bar;
                st.ref_wait_since_ms = now_ms;
            }
            if (now_ms - st.ref_wait_since_ms < kReferenceWaitMs) {
                st.ref_waiting = true;
                return decline("awaiting the information venues' decision bar");
            }
        }
        st.ref_waiting = false;
    }
    st.last_emitted_revision = revision;

    DecisionContext c;
    c.canonical_pair_id = pair;
    c.bitget_instrument_id = pair;
    c.event_time_ms = st.bitget_features.bar_open_time_ms;
    c.available_at_ms = now_ms;
    c.expires_at_ms = now_ms + cfg_.context_ttl_ms;
    c.context_revision = revision;
    c.global_universe_status = st.universe_status;
    c.data_quality = st.quality;
    c.venue_mask = st.venue_mask;
    c.missing_venue_mask = st.missing_venue_mask;
    c.regime_divergence = st.regime_divergence;
    c.bitget_features = st.bitget_features;
    c.cross_venue_features = st.cross_venue_features;
    c.reference_features = st.reference_features;

    // Higher-timeframe context, admitted one grid at a time under the closed-bar
    // rule: a bar of length S that opened at O is only observable from O + S, so
    // it may enter a decision at T0 only when O + S <= T0. Joining on open time
    // would hand a strategy an hourly bar that has not finished forming -- the
    // most expensive kind of leakage, because it looks like context and behaves
    // like the answer. A grid failing either test is ABSENT, never replaced by
    // the execution timeframe's values.
    const int64_t context_age_limit =
        cfg_.max_context_age_ms > 0 ? cfg_.max_context_age_ms : cfg_.max_component_age_ms;
    for (const auto& [tf_minutes, summary] : st.context_features) {
        const char* skip = nullptr;
        int64_t close_ms = 0;
        if (tf_minutes <= 0) skip = "bad_timeframe";
        else if (!summary.valid) skip = "not_valid";
        else if (summary.values.empty()) skip = "no_values";
        else {
            close_ms = summary.bar_open_time_ms + static_cast<int64_t>(tf_minutes) * 60'000;
            if (close_ms > now_ms) skip = "not_yet_closed";
            else if (now_ms - close_ms > context_age_limit) skip = "stale";
        }
        if (skip) {
            // Rate-limited to once per pair+grid+reason so a persistently
            // rejected grid is visible without flooding: a grid silently
            // missing from every context is indistinguishable from one that
            // never arrived, which cost real time to diagnose.
            const auto key = std::to_string(pair) + ":" + std::to_string(tf_minutes) + ":" + skip;
            if (grid_skip_logged_.insert(key).second) {
                log::warn("context_grid_skipped",
                          {{"pair", pair},
                           {"timeframe_minutes", tf_minutes},
                           {"reason", skip},
                           {"age_sec", close_ms ? (now_ms - close_ms) / 1000 : 0},
                           {"limit_sec", context_age_limit / 1000}});
            }
            continue;
        }
        c.context_features[tf_minutes] = summary;
    }
    // The microstructure block enters a decision only while it is FRESH.
    //
    // The higher grids above are age-checked and skipped with a reason when
    // stale; this block was copied unconditionally, so a stalled window feed
    // would have kept serving the last CVD, trade imbalance and book reading
    // into every context indefinitely -- indistinguishable, downstream, from a
    // market that had gone quiet. Spread and depth were already age-gated
    // separately; the values were not.
    // Age from the window's CLOSE, not its start.
    const int64_t micro_span = window_span_ms(cfg_.microstructure_window);
    const int64_t micro_close_ms = st.micro_features.bar_open_time_ms + micro_span;
    const bool micro_usable =
        st.micro_features.valid && st.micro_features.bar_open_time_ms > 0 &&
        now_ms - micro_close_ms <= cfg_.max_microstructure_age_ms;
    if (micro_usable) {
        c.microstructure = st.micro_features;
    } else if (st.micro_features.bar_open_time_ms > 0) {
        const auto key = std::to_string(pair) + ":micro:stale";
        if (grid_skip_logged_.insert(key).second) {
            log::warn("microstructure_skipped",
                      {{"pair", pair},
                       {"reason", "stale"},
                       {"age_sec", (now_ms - micro_close_ms) / 1000},
                       {"limit_sec", cfg_.max_microstructure_age_ms / 1000}});
        }
    }
    c.bitget_regime = st.bitget_regime;
    c.global_regime = effective_regime;
    // Bitget's 15m / 1h regime, admitted only while it describes a bar that
    // closed at most two of its own bars ago and never one still forming.
    for (const auto& [tf, view] : st.htf_regimes) {
        if (tf <= 0 || view.bar_open_time_ms <= 0) continue;
        const int64_t span = static_cast<int64_t>(tf) * 60'000;
        const int64_t close_ms = view.bar_open_time_ms + span;
        if (close_ms > now_ms || now_ms - close_ms > 2 * span) continue;
        c.bitget_htf_regime[tf] = view;
    }
    c.bitget_projected_zones = st.zones;
    c.market_forecast = st.forecast;
    // Econ estimates, each at most max_forecast_age from its bar close; the
    // economic policy re-checks the age at decision time. An estimate from a
    // bar that has not closed yet is impossible from a live producer and is
    // refused as such.
    for (const auto& [h, f] : st.forecasts) {
        const int64_t close_ms = f.bar_open_time_ms + 60'000;
        if (!f.present || close_ms > now_ms + 5'000) continue;
        if (now_ms - close_ms > cfg_.max_forecast_age_ms) continue;
        c.forecasts[h] = f;
    }
    c.feature_manifest_hash = st.feature_manifest_hash.empty() ? "altstrategy-fs-v1" : st.feature_manifest_hash;
    c.fees_bps = cfg_.default_fees_bps;

    // Every field below is taken from the source that actually measures it.
    // None of them are TA-Lib outputs, so none of them can come from the
    // `talib-core` vector — that vector holds no price, spread, depth or
    // funding member at all.
    //
    //  price   : cross-venue consensus mid, the four-venue estimate
    //  spread  : Bitget's own book, from the microstructure window
    //  depth   : top-of-book bid + ask notional, same window
    //  funding : cross-venue consensus 8h funding
    const bool micro_fresh = micro_usable;

    // `mark_price` carries the consensus MID: it is what last_price() reads, and
    // a mid is the right answer to "what is this trading at". The mark and the
    // index are separate members of the same vector and are carried separately,
    // because a basis is (mark - index) and computing it from a mid is not a
    // small error -- the mid does not contain the funding mechanics that make a
    // basis exist. index_price was declared on the context and never assigned by
    // anything, so every strategy gated on `index_price > 0` was unreachable.
    // Accumulated OI growth over the crowding window, compounded from the
    // minute deltas. ABSENT when too little of the window was observed: a
    // partial product understates the growth, and an understated crowding
    // reading is the kind of number that passes a gate it should not.
    if (st.oi_changes.size() >= kOiCrowdingMinSamples) {
        double compounded = 1.0;
        for (const auto& [when, r] : st.oi_changes) {
            (void)when;
            compounded *= (1.0 + r);
        }
        const double growth = compounded - 1.0;
        if (std::isfinite(growth)) c.cross_venue_features.values["oi_growth_1h"] = growth;
    }

    // Cross-venue agreement WITH A DIRECTION.
    //
    // AltTech's venue_momentum_agreement and orderflow_agreement are SHARES --
    // the fraction of venues whose 1m return (or signed flow) has the same sign
    // as the consensus -- on [0,1] whatever the direction; the direction itself
    // is consensus_momentum_score (the consensus 1m log return) and
    // cross_venue_signed_volume. The strategies read the shares as signed
    // quantities (`> 0.1` for a long, `< -0.1` for a short). Over 14 days the
    // share never went below 0.25, so every `<= 0` / `< 0` short gate was
    // unreachable -- donchian, ema_compression, multi_timeframe and the
    // volume burst produced longs only -- and momentum_continuation's
    // "regime TREND_UP OR agreement > 0.1" admitted longs in any regime (394 in
    // TREND_DOWN). The signed agreement is sign(direction) x share; absent when
    // either input is. The shares stay under their own names.
    const auto signed_share = [&](const char* direction, const char* share, const char* out) {
        const auto& xv = c.cross_venue_features;
        if (!xv.has(direction) || !xv.has(share)) return;
        const double d = xv.get(direction), s = xv.get(share);
        if (!std::isfinite(d) || !std::isfinite(s)) return;
        const double sign = d > 0 ? 1.0 : (d < 0 ? -1.0 : 0.0);
        c.cross_venue_features.values[out] = sign * s;
    };
    signed_share("consensus_momentum_score", "momentum_agreement", "momentum_direction_agreement");
    signed_share("consensus_flow", "breakout_agreement", "flow_direction_agreement");

    // The same question asked on the DECISION grid.
    //
    // momentum_direction_agreement is built from AltTech's cross-venue
    // consensus, which is a 1m product: the sign of the last MINUTE's consensus
    // return and the share of venues agreeing with it. Setups on the 15m grid
    // read it as confirmation of a 15m move -- a one-minute reading, taken
    // three to five minutes after the 15m bar closed, standing in for the bar.
    // Here the four venues' own 15m roc_5 (each from its own 15m candles) give
    // the 15m consensus direction and the share of venues agreeing with it.
    // Absent unless Bitget and at least two information venues were observed
    // on this grid -- three of four, the same floor as a degraded context.
    {
        std::vector<double> rocs;
        if (c.bitget_features.has("roc_5") && std::isfinite(c.bitget_features.get("roc_5"))) {
            rocs.push_back(c.bitget_features.get("roc_5"));
            for (const auto& [venue, ref] : c.reference_features) {
                if (ref.valid && ref.bar_open_time_ms == c.bitget_features.bar_open_time_ms &&
                    ref.has("roc_5") && std::isfinite(ref.get("roc_5"))) {
                    rocs.push_back(ref.get("roc_5"));
                }
            }
        }
        if (rocs.size() >= 3) {
            double mean = 0;
            for (const double r : rocs) mean += r;
            mean /= static_cast<double>(rocs.size());
            const double dir = mean > 0 ? 1.0 : (mean < 0 ? -1.0 : 0.0);
            double agree = 0;
            for (const double r : rocs) {
                if ((r > 0 ? 1.0 : (r < 0 ? -1.0 : 0.0)) == dir) agree += 1.0;
            }
            c.bitget_features.values["momentum_direction_agreement_15m"] =
                dir * agree / static_cast<double>(rocs.size());
        }
    }

    c.mark_price = st.cross_venue_features.get("consensus_mid_price", 0.0);
    c.index_price = st.cross_venue_features.get("consensus_index_price", 0.0);
    c.funding_rate = st.cross_venue_features.get("funding_rate_8h", 0.0);
    // AltTech's bitget_liquidity_rank is an ORDINAL rank among the venues that
    // quoted — 1 is the tightest spread — not a [0,1] score. 0 means absent.
    c.liquidity_rank = st.cross_venue_features.get("liquidity_rank", 0.0);

    if (micro_fresh) {
        // `spread_bps` is the LOCAL name: the microstructure vector is
        // positional, and altstrategy.yaml maps AltTech's book_spread_bps
        // (position 18) onto it. Asking for the producer-side name here would
        // miss every time.
        //
        // The flag matters because for months this column recorded the fallback
        // 10.0 as though it were an observed spread -- 2 430 of 2 484 contexts
        // in a day -- while no order book was being persisted at all. The number
        // was indistinguishable from a measurement, and a defaulting lookup
        // cannot report that it defaulted. Now it can.
        // The last reading at or before T0, provided it is still inside the
        // freshness window the config allows.
        const bool book_fresh = st.last_book_obs_ms > 0 &&
                                now_ms - st.last_book_obs_ms <= cfg_.max_microstructure_age_ms;
        c.spread_observed = book_fresh && st.last_spread_bps > 0.0;
        c.spread_bps = c.spread_observed ? st.last_spread_bps : kFallbackSpreadBps;
        c.depth_observed = book_fresh && st.last_top_depth_usdt > 0.0;
        c.top_depth_usdt = c.depth_observed ? st.last_top_depth_usdt : kFallbackTopDepthUsdt;
    } else {
        // No recent book reading: state the conservative default rather than
        // reusing a stale one. A plan sized against a spread from ten minutes
        // ago is a plan for a market that no longer exists.
        c.spread_observed = false;
        c.depth_observed = false;
        c.spread_bps = kFallbackSpreadBps;
        c.top_depth_usdt = kFallbackTopDepthUsdt;
    }

    // Expected slippage is not published by any producer; it is derived from
    // what is. Crossing the book costs half the quoted spread, plus an impact
    // term for how wide Bitget's book is against the other venues.
    //
    // The rank is ordinal over the four-venue universe: rank 1 (tightest) pays
    // no impact, the widest pays all of it. An absent rank is UNKNOWN, not good,
    // so it is charged the midpoint rather than silently treated as best.
    const double impact_fraction =
        c.liquidity_rank >= 1.0
            ? std::clamp((c.liquidity_rank - 1.0) / (kVenueUniverseSize - 1.0), 0.0, 1.0)
            : 0.5;
    c.expected_slippage_bps = 0.5 * c.spread_bps + kDepthImpactBps * impact_fraction;

    // Volume surge, on a scale the strategy thresholds can actually mean
    // something on.
    //
    // AltTech publishes no z-scored volume. What it has is
    // `volume_acceleration`: this window's volume divided by the previous
    // window's — a raw ratio whose distribution is violently heavy-tailed
    // (measured live: median 0.68, p90 15, p99 380, max 120547, because the
    // denominator is often near zero). Feeding that straight into thresholds
    // written in sigma units — reject below 1, `vol_z_min` 2, "above 3 is a
    // scalp" — compares a ratio against a standard deviation and classified a
    // quarter of all signals as scalps off a division artifact.
    //
    // The log of the ratio is the natural fix: it makes a multiplicative
    // quantity additive and symmetric, so 0 is "unchanged", 1 is "e times the
    // previous window", 3 is "twenty times". Against the measured distribution
    // that puts p75 at 1.2 and p90 at 2.7, which is what the existing 1/2/3
    // thresholds were reaching for. A ratio of zero carries no information
    // about magnitude, so it is floored rather than sent to -infinity.
    // Microprice deviation, in BASIS POINTS.
    //
    // AltTech publishes it as (microprice - mid) / mid -- a ratio. Two strategies
    // read it under a comment saying "bps" and compared it against thresholds of
    // 0.5 and 3.0 bps, so they were testing a ratio against a basis-point number
    // and were short by a factor of 10 000: measured here the ratio's maximum
    // over two hours was 0.000446, which is 4.46 bps and would have passed, while
    // the raw value could not reach 0.5 on any book that has ever existed. The
    // unit now lives in the name so the next reader cannot make the same
    // assumption.
    if (c.microstructure.has("book_microprice_dev")) {
        const double ratio = c.microstructure.get("book_microprice_dev");
        if (std::isfinite(ratio)) {
            c.microstructure.values["book_microprice_dev_bps"] = ratio * 10'000.0;
        }
    }

    if (c.microstructure.has("turnover_acceleration")) {
        const double ratio = c.microstructure.get("turnover_acceleration");
        c.microstructure.values["volume_surge"] =
            ratio > 0.0 ? std::max(std::log(ratio), kMinVolumeSurge) : kMinVolumeSurge;
    }
    derive_engine_features(c.bitget_features, c.microstructure, c.last_price());

    // Advance the VWAP-side history, then answer the reclaim question with it.
    //
    // Only closed bars enter, and only when the bar is newer than the one the
    // side was last read at, so re-building the same bar cannot manufacture a
    // crossing. `vwap_dev` is absent whenever the microstructure window carried
    // no VWAP, and an absent deviation leaves the history untouched rather than
    // recording a side of zero.
    if (c.bitget_features.has("vwap_dev") &&
        c.bitget_features.bar_open_time_ms > st.vwap_side_bar_ms) {
        const double dev = c.bitget_features.get("vwap_dev");
        const int side = dev > 0 ? 1 : (dev < 0 ? -1 : 0);
        if (side != 0) {
            if (st.vwap_side != 0 && side != st.vwap_side)
                st.vwap_flip_bar_ms = c.bitget_features.bar_open_time_ms;
            st.vwap_side = side;
        }
        st.vwap_side_bar_ms = c.bitget_features.bar_open_time_ms;
    }
    if (st.vwap_flip_bar_ms > 0 && decision_span_ms > 0) {
        const int64_t age_bars =
            (c.bitget_features.bar_open_time_ms - st.vwap_flip_bar_ms) / decision_span_ms;
        c.bitget_features.values["vwap_lost_recently"] =
            (age_bars >= 0 && age_bars <= kVwapReclaimBars) ? 1.0 : 0.0;
    }
    // The prior-bar Donchian channel, only for the bar immediately before this
    // one: a channel from further back is a different channel.
    if (st.prev_exec_bar_ms > 0 && decision_span_ms > 0 &&
        c.bitget_features.bar_open_time_ms - st.prev_exec_bar_ms == decision_span_ms &&
        std::isfinite(st.prev_donchian_high) && std::isfinite(st.prev_donchian_low) &&
        st.prev_donchian_high > 0 && st.prev_donchian_low > 0) {
        c.bitget_features.values["donchian_high_20_prev"] = st.prev_donchian_high;
        c.bitget_features.values["donchian_low_20_prev"] = st.prev_donchian_low;
    }

    // The session VWAP (SessionVwap), while its windows are still arriving: a
    // session level last updated long ago is not the level as of T0.
    const int64_t micro_span_ms = window_span_ms(cfg_.microstructure_window);
    const bool session_fresh =
        st.session_vwap.last_window_ms > 0 && micro_span_ms > 0 &&
        now_ms - (st.session_vwap.last_window_ms + micro_span_ms) <= cfg_.max_microstructure_age_ms;
    const double px_now = c.last_price();
    if (const auto sv = st.session_vwap.value(); sv && session_fresh && px_now > 0) {
        c.bitget_features.values["session_vwap"] = *sv;
        c.bitget_features.values["session_vwap_dev_bps"] = (px_now - *sv) / *sv * 10'000.0;
    }
    // The reclaim question against the session VWAP, answered exactly as the
    // one above: sides read only from closed bars, each bar once.
    if (c.bitget_features.has("session_vwap_dev_bps") &&
        c.bitget_features.bar_open_time_ms > st.session_vwap_side_bar_ms) {
        const double dev = c.bitget_features.get("session_vwap_dev_bps");
        const int side = dev > 0 ? 1 : (dev < 0 ? -1 : 0);
        if (side != 0) {
            if (st.session_vwap_side != 0 && side != st.session_vwap_side)
                st.session_vwap_flip_bar_ms = c.bitget_features.bar_open_time_ms;
            st.session_vwap_side = side;
        }
        st.session_vwap_side_bar_ms = c.bitget_features.bar_open_time_ms;
    }
    if (st.session_vwap_flip_bar_ms > 0 && decision_span_ms > 0 &&
        c.bitget_features.has("session_vwap_dev_bps")) {
        const int64_t age_bars =
            (c.bitget_features.bar_open_time_ms - st.session_vwap_flip_bar_ms) / decision_span_ms;
        c.bitget_features.values["session_vwap_lost_recently"] =
            (age_bars >= 0 && age_bars <= kVwapReclaimBars) ? 1.0 : 0.0;
    }
    c.context_id = ids::make("ctx", ids::join({ids::i64(pair), ids::i64(revision)}));
    st.last_built = c;
    return c;
}

std::optional<DecisionContext> ContextAssembler::state_snapshot(int64_t pair, int64_t now_ms) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = pairs_.find(pair);
    if (it == pairs_.end() || !it->second.last_built) return std::nullopt;
    const PairState& st = it->second;
    if (now_ms - st.last_built->available_at_ms > cfg_.state_max_context_age_ms) return std::nullopt;
    DecisionContext c = *st.last_built;
    c.forecasts.clear();
    int64_t latest_close = 0;
    for (const auto& [h, f] : st.forecasts) {
        const int64_t close_ms = f.bar_open_time_ms + 60'000;
        if (!f.present || close_ms > now_ms + 5'000) continue;
        if (now_ms - close_ms > cfg_.max_forecast_age_ms) continue;
        c.forecasts[h] = f;
        latest_close = std::max(latest_close, close_ms);
    }
    if (c.forecasts.empty()) return std::nullopt;
    c.market_forecast = st.forecast;
    const double mid = st.cross_venue_features.get("consensus_mid_price", 0.0);
    if (mid > 0) c.mark_price = mid;
    const double idx = st.cross_venue_features.get("consensus_index_price", 0.0);
    if (idx > 0) c.index_price = idx;
    c.funding_rate = st.cross_venue_features.get("funding_rate_8h", c.funding_rate);
    c.event_time_ms = latest_close;
    c.available_at_ms = now_ms;
    c.expires_at_ms = now_ms + cfg_.context_ttl_ms;
    return c;
}

} // namespace altstrategy::engine_io
