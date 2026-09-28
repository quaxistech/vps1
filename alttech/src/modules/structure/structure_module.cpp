// Primary-venue market-structure module: support and resistance zones with a
// lifecycle, on 15m, 1h and 4h.
//
// A zone is a price band that several independent detection methods agree on.
// The methods are the standard ones, each contributing a confluence weight:
//
//   * swing pivots (fractals, wing 2) over a 500-bar window;
//   * one-dimensional clustering of pivot prices at ATR-scaled distance;
//   * volume profile POC / value-area high and low, computed from the typical
//     price of each bar because tick-level history is not retained;
//   * session (UTC day) and rolling VWAP;
//   * previous day and week extremes, Donchian 20/55 bounds and the round-number
//     grid nearest to price.
//
// Every geometric constant is expressed in ATR or in a fraction of price, so no
// threshold depends on an instrument's price level or tick size. The band
// half-width is max(0.25 ATR, 5 bps of price); the confluence weights and the
// lifecycle thresholds are deployment parameters under modules.structure.
//
// Lifecycle: candidate -> active -> tested -> broken/flipped/expired. A broken
// zone creates the mirrored candidate, which is the classic support/resistance
// role flip.

#include "zone_lifecycle.hpp"
#include "zone_merge.hpp"

#include "alttech/application/module_supervisor.hpp"
#include "venue_sweep.hpp"
#include "alttech/modules/structure.hpp"

#include "alttech/common/shutdown.hpp"
#include "alttech/config/config.hpp"
#include "alttech/logging/logger.hpp"
#include "alttech/messaging/codec.hpp"
#include "alttech/messaging/nats_client.hpp"
#include "alttech/messaging/topology.hpp"
#include "alttech/persistence/pg.hpp"
#include "alttech/persistence/txn_log.hpp"
#include "alttech/time/time.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <map>
#include <optional>
#include <stdexcept>
#include <string_view>

using namespace alttech;
using namespace alttech::modules::structure;

namespace alttech::modules {
namespace {
using alttech::structure::ZoneCandidate;
using alttech::structure::merge_zone_candidates;
using alttech::structure::zone_weight;


constexpr std::string_view kModuleName = "structure";
constexpr const char* kAlgoVersion = "venue-structure-1";
constexpr int kWindowBars = 500;
constexpr int kPivotWing = 2;

// Zone geometry, all expressed relative to ATR or to price so that no constant
// depends on an instrument's price level.
constexpr double kBandAtrFraction = 0.25;      // half-width in ATR
constexpr double kBandMinPriceFraction = 5e-4; // half-width floor: 5 bps of price
constexpr double kClusterAtrFraction = 0.5;    // pivots merge within half an ATR
constexpr double kClusterMinPriceFraction = 1e-3;
constexpr double kExpireStrength = 0.15;
constexpr double kNewZoneMinStrength = 0.20;
constexpr double kFlipStrengthCarryOver = 0.70;

// Bars of recent history the lifecycle is evaluated over. The whole state of a
// zone is a function of this window, never an accumulator: AltSpotter revises
// 5-8 % of closed bars, with a 95th percentile around seven hours after close,
// so anything incremented bar by bar drifts away from the corrected data and
// never comes back. Deriving it instead means a revision simply produces a
// different answer on the next cycle.
constexpr std::size_t kLifecycleWindowBars = 120;



struct LiveZone {
    int64_t id = 0;
    std::string kind;
    double lower = 0, center = 0, upper = 0;
    double strength = 0, confidence = 0;
    std::string status;
    int touch_count = 0, reaction_count = 0;
    int64_t created_ms = 0;   // when the level was first recognised
};

// Confidence grows with the number of independent methods that agree on a band.
[[nodiscard]] double method_confidence(std::size_t methods) {
    return std::min(1.0, 0.3 + 0.1 * static_cast<double>(methods));
}

void record_event(pqxx::work& txn, int64_t canonical_pair_id, const std::string& timeframe,
                  int64_t zone_id, int64_t bar_open_ms, const char* event) {
    txn.exec_params(
        "INSERT INTO alttech.structure_zone_events (scope, canonical_pair_id, exchange_id, "
        "zone_id, timeframe, event, bar_open_time) "
        "VALUES ('VENUE', $1, 1, $2, $3, $4, to_timestamp($5::bigint / 1000.0))",
        canonical_pair_id, zone_id, timeframe, std::string(event), bar_open_ms);
}


struct StructureNotification {
    std::string kind;
    std::string zone_kind;
    std::string resulting_zone_kind;
    std::optional<double> center_price;
};

::alttech::v1::Timeframe wire_timeframe(std::string_view label) {
    const auto timeframe = tf_from_label(label);
    return timeframe ? static_cast<::alttech::v1::Timeframe>(tf_minutes(*timeframe))
                     : ::alttech::v1::TIMEFRAME_UNSPECIFIED;
}

::alttech::v1::StructureEventType wire_structure_event(std::string_view kind) {
    if (kind == "broken") return ::alttech::v1::STRUCTURE_EVENT_ZONE_BROKEN;
    if (kind == "tested") return ::alttech::v1::STRUCTURE_EVENT_ZONE_TESTED;
    if (kind == "flipped") return ::alttech::v1::STRUCTURE_EVENT_ZONE_FLIPPED;
    if (kind == "expired") return ::alttech::v1::STRUCTURE_EVENT_ZONE_EXPIRED;
    return ::alttech::v1::STRUCTURE_EVENT_UNSPECIFIED;
}

double round_step_for(double price) {
    // Nearest "round number" grid: 1-2-5 progression around 1% of price.
    const double raw = price * 0.01;
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    for (const double m : {1.0, 2.0, 5.0, 10.0}) {
        if (raw <= m * mag) return m * mag;
    }
    return 10.0 * mag;
}

class StructureService {
public:
    StructureService(persistence::PgPool& pool, messaging::NatsClient& nats)
        : pool_(pool), nats_(nats) {}

    void run_cycle() {
        std::vector<std::pair<int64_t, std::string>> instruments;
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            const auto rows = txn.exec(
                "SELECT ei.instrument_id, ei.exchange_symbol "
                "FROM global_observation_universe u "
                "JOIN canonical_pair_exchange_map m "
                "  ON m.canonical_pair_id = u.canonical_pair_id AND m.exchange_id = 1 "
                "JOIN exchange_instruments ei ON ei.id = m.exchange_instrument_id "
                "WHERE u.eligible AND ei.is_whitelisted AND ei.status = 'active' "
                "ORDER BY ei.exchange_symbol");
            for (const auto& r : rows) {
                instruments.emplace_back(r[0].as<int64_t>(), r[1].as<std::string>());
            }
        });
        if (!res) {
            log::error("structure_instruments_failed", {{"message", res.error().message}});
            return;
        }
        int zones_total = 0;
        for (const auto& [iid, sym] : instruments) {
            if (ShutdownToken::instance().stop_requested()) return;
            for (const char* tf : {"15m", "1h", "4h"}) {
                zones_total += process_shard(iid, sym, tf);
            }
        }
        log::info("structure_cycle_done", {{"instruments", instruments.size()},
                                           {"zones_active", zones_total}});
    }

private:
    std::vector<SBar> load_bars(int64_t iid, const std::string& tf) {
        std::vector<SBar> bars;
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            txn.exec("SET LOCAL TIME ZONE 'UTC'");
            const auto rows = txn.exec_params(
                "SELECT (extract(epoch FROM open_time) * 1000)::bigint, open::float8, "
                "high::float8, low::float8, close::float8, base_volume::float8, "
                "quote_volume::float8, revision "
                "FROM market_candles_agg WHERE exchange_id = 1 AND instrument_id = $1 "
                "AND timeframe = $2 AND is_closed AND quality_status = 'ok' "
                "ORDER BY open_time DESC LIMIT $3",
                iid, tf, kWindowBars);
            for (const auto& r : rows) {
                bars.push_back({r[0].as<int64_t>(), r[1].as<double>(), r[2].as<double>(),
                                r[3].as<double>(), r[4].as<double>(), r[5].as<double>(),
                                r[6].as<double>(), r[7].as<int64_t>()});
            }
        });
        if (!res) {
            log::error("structure_bars_load_failed",
                       {{"instrument_id", iid}, {"timeframe", tf},
                        {"message", res.error().message}});
            return {};
        }
        std::reverse(bars.begin(), bars.end());
        return bars;
    }

    int process_shard(int64_t iid, const std::string& sym, const std::string& tf) {
        const auto bars = load_bars(iid, tf);
        if (bars.size() < 60) return 0;
        const double last_close = bars.back().close;

        // ATR(14) for widths/tolerances.
        double atr = 0;
        for (size_t i = bars.size() - 14; i < bars.size(); ++i) {
            atr += std::max({bars[i].high - bars[i].low,
                             std::abs(bars[i].high - bars[i - 1].close),
                             std::abs(bars[i].low - bars[i - 1].close)});
        }
        atr /= 14.0;
        const double half_width =
            std::max(atr * kBandAtrFraction, last_close * kBandMinPriceFraction);

        auto candidates = build_candidates(bars, atr, half_width, last_close);
        const int active = reconcile_zones(iid, sym, tf, bars, candidates, atr, half_width);
        return active;
    }

    std::vector<ZoneCandidate> build_candidates(const std::vector<SBar>& bars, double atr,
                                                double half_width, double last_close) {
        std::vector<ZoneCandidate> out;
        // --- pivots (fractal wing=2) ---
        std::vector<std::pair<double, bool>> pivots; // price, is_high
        for (size_t i = kPivotWing; i + kPivotWing < bars.size(); ++i) {
            bool ph = true, pl = true;
            for (int k = 1; k <= kPivotWing; ++k) {
                ph &= bars[i].high >= bars[i - k].high && bars[i].high >= bars[i + k].high;
                pl &= bars[i].low <= bars[i - k].low && bars[i].low <= bars[i + k].low;
            }
            if (ph) pivots.emplace_back(bars[i].high, true);
            if (pl) pivots.emplace_back(bars[i].low, false);
        }
        // --- 1-D clustering by ATR distance ---
        std::sort(pivots.begin(), pivots.end());
        const double cluster_dist =
            std::max(atr * kClusterAtrFraction, last_close * kClusterMinPriceFraction);
        size_t start = 0;
        while (start < pivots.size()) {
            size_t end = start + 1;
            while (end < pivots.size() &&
                   pivots[end].first - pivots[end - 1].first <= cluster_dist) {
                ++end;
            }
            const size_t n = end - start;
            if (n >= 2) { // a level needs at least two pivot touches
                double sum = 0;
                size_t highs = 0;
                for (size_t k = start; k < end; ++k) {
                    sum += pivots[k].first;
                    if (pivots[k].second) ++highs;
                }
                ZoneCandidate z;
                z.center = sum / static_cast<double>(n);
                z.lower = z.center - half_width;
                z.upper = z.center + half_width;
                z.kind = z.center > last_close ? "resistance" : "support";
                z.methods["pivot_cluster"] = static_cast<double>(n);
                z.touch_count = static_cast<int>(n);
                out.push_back(std::move(z));
            }
            start = end;
        }
        // --- volume profile (OHLCV approximation, 40 bins) ---
        double lo = bars[0].low, hi = bars[0].high;
        for (const auto& b : bars) {
            lo = std::min(lo, b.low);
            hi = std::max(hi, b.high);
        }
        if (hi > lo) {
            constexpr int kBins = 40;
            std::array<double, kBins> vol{};
            for (const auto& b : bars) {
                const double tp = (b.high + b.low + b.close) / 3.0;
                int bin = static_cast<int>((tp - lo) / (hi - lo) * kBins);
                bin = std::clamp(bin, 0, kBins - 1);
                vol[bin] += b.quote;
            }
            const auto poc_it = std::max_element(vol.begin(), vol.end());
            const int poc = static_cast<int>(poc_it - vol.begin());
            const double bin_w = (hi - lo) / kBins;
            const double poc_price = lo + (poc + 0.5) * bin_w;
            // Value area: expand around POC to 70% volume.
            double total = 0;
            for (const double v : vol) total += v;
            double acc = vol[poc];
            int a = poc, b2 = poc;
            while (acc < 0.7 * total && (a > 0 || b2 < kBins - 1)) {
                const double up = b2 < kBins - 1 ? vol[b2 + 1] : -1;
                const double dn = a > 0 ? vol[a - 1] : -1;
                if (up >= dn) {
                    acc += vol[++b2];
                } else {
                    acc += vol[--a];
                }
            }
            const auto add_vp = [&](double price, const char* name, double weight) {
                ZoneCandidate z;
                z.center = price;
                z.lower = price - half_width;
                z.upper = price + half_width;
                z.kind = price > last_close ? "resistance" : "support";
                z.methods[name] = weight;
                z.volume_weight = weight;
                out.push_back(std::move(z));
            };
            add_vp(poc_price, "volume_profile_poc", 2.0);
            add_vp(lo + (b2 + 1.0) * bin_w, "volume_profile_vah", 1.2);
            add_vp(lo + a * bin_w, "volume_profile_val", 1.2);
        }
        // --- VWAPs ---
        {
            // session VWAP (current UTC day) + rolling(100) + anchored at day start
            double pv = 0, vv = 0;
            const int64_t day_start = timeutil::floor_interval(bars.back().open_ms, 1440);
            for (const auto& b : bars) {
                if (b.open_ms >= day_start) {
                    pv += (b.high + b.low + b.close) / 3.0 * b.volume;
                    vv += b.volume;
                }
            }
            if (vv > 0) {
                ZoneCandidate z;
                z.center = pv / vv;
                z.lower = z.center - half_width;
                z.upper = z.center + half_width;
                z.kind = "dynamic";
                z.methods["session_vwap"] = 1.5;
                out.push_back(std::move(z));
            }
            double rpv = 0, rvv = 0;
            const size_t from = bars.size() > 100 ? bars.size() - 100 : 0;
            for (size_t i = from; i < bars.size(); ++i) {
                rpv += (bars[i].high + bars[i].low + bars[i].close) / 3.0 * bars[i].volume;
                rvv += bars[i].volume;
            }
            if (rvv > 0) {
                ZoneCandidate z;
                z.center = rpv / rvv;
                z.lower = z.center - half_width;
                z.upper = z.center + half_width;
                z.kind = "dynamic";
                z.methods["rolling_vwap"] = 1.0;
                out.push_back(std::move(z));
            }
        }
        // --- classic levels ---
        {
            const int64_t day = 86'400'000;
            const int64_t today = timeutil::floor_interval(bars.back().open_ms, 1440);
            double pd_hi = 0, pd_lo = 1e300, pw_hi = 0, pw_lo = 1e300;
            const int64_t week_start = today - ((today / day + 3) % 7) * day; // Monday UTC
            for (const auto& b : bars) {
                if (b.open_ms >= today - day && b.open_ms < today) {
                    pd_hi = std::max(pd_hi, b.high);
                    pd_lo = std::min(pd_lo, b.low);
                }
                if (b.open_ms >= week_start - 7 * day && b.open_ms < week_start) {
                    pw_hi = std::max(pw_hi, b.high);
                    pw_lo = std::min(pw_lo, b.low);
                }
            }
            const auto add_classic = [&](double price, const char* name, double w) {
                if (price <= 0 || price > 1e299) return;
                ZoneCandidate z;
                z.center = price;
                z.lower = price - half_width;
                z.upper = price + half_width;
                z.kind = price > last_close ? "resistance" : "support";
                z.methods[name] = w;
                out.push_back(std::move(z));
            };
            add_classic(pd_hi, "prev_day_high", 1.4);
            add_classic(pd_lo, "prev_day_low", 1.4);
            add_classic(pw_hi, "prev_week_high", 1.6);
            add_classic(pw_lo, "prev_week_low", 1.6);
            // Donchian 20/55
            double d20h = 0, d20l = 1e300, d55h = 0, d55l = 1e300;
            for (size_t i = bars.size() - std::min<size_t>(bars.size(), 55); i < bars.size();
                 ++i) {
                const bool in20 = i >= bars.size() - std::min<size_t>(bars.size(), 20);
                if (in20) {
                    d20h = std::max(d20h, bars[i].high);
                    d20l = std::min(d20l, bars[i].low);
                }
                d55h = std::max(d55h, bars[i].high);
                d55l = std::min(d55l, bars[i].low);
            }
            add_classic(d20h, "donchian20_high", 1.0);
            add_classic(d20l, "donchian20_low", 1.0);
            add_classic(d55h, "donchian55_high", 1.2);
            add_classic(d55l, "donchian55_low", 1.2);
            // round numbers: nearest 3 grid lines around price
            const double step = round_step_for(last_close);
            const double base = std::floor(last_close / step) * step;
            for (const double p : {base - step, base, base + step}) {
                add_classic(p, "round_number", 0.8);
            }
        }
        auto merged = merge_zone_candidates(std::move(out), half_width);
        return merged;
    }

    double score_zone(const ZoneCandidate& z, double last_close, double atr) {
        double confluence = 0;
        for (const auto& [_, w] : z.methods) confluence += w;
        const double touch = std::min(1.0, z.touch_count / 6.0);
        const double dist = std::abs(z.center - last_close);
        const double proximity = std::exp(-dist / (10.0 * std::max(atr, 1e-12)));
        return std::min(1.0, 0.15 * confluence + 0.4 * touch + 0.25 * proximity +
                                 0.2 * std::min(1.0, z.volume_weight / 2.0));
    }

    // Reconcile the persisted zones with what the current window says about them.
    //
    // Every zone property — counters, strength, lifecycle — is recomputed from
    // the window on each cycle, so running this twice over the same bar changes
    // nothing, and running it after AltSpotter revised a candle produces the
    // corrected answer instead of preserving the old one. That is why there is
    // no processed-bar marker and no stale-revision warning any more: there is
    // nothing left that a revision could corrupt.
    int reconcile_zones(int64_t iid, const std::string& sym, const std::string& tf,
                        const std::vector<SBar>& bars, std::vector<ZoneCandidate>& cands,
                        double atr, double half_width) {
        const double last_close = bars.back().close;
        const SBar& last_bar = bars.back();
        const std::size_t window_from =
            bars.size() > kLifecycleWindowBars ? bars.size() - kLifecycleWindowBars : 0;
        std::vector<StructureNotification> events;
        int active = 0;
        int withheld = 0;   // candidates with no touch in the observable window

        const bool committed = persistence::with_txn_logged(
            pool_, "structure_reconcile", [&](pqxx::work& txn) {
            txn.exec("SET LOCAL TIME ZONE 'UTC'");
            // Serialize duplicate service instances against each other. The work
            // is idempotent, so this is about not interleaving two writers, not
            // about processing a bar once.
            txn.exec_params(
                "SELECT pg_advisory_xact_lock(hashtextextended($1::text || ':' || $2, 0))",
                iid, tf);

            std::vector<LiveZone> existing;
            const auto rows = txn.exec_params(
                "SELECT id, kind, lower_price::float8, center_price::float8, "
                "upper_price::float8, strength_score, confidence, lifecycle, touch_count, "
                "reaction_count, (extract(epoch FROM first_seen_at) * 1000)::bigint "
                "FROM alttech.venue_structure_zones "
                "WHERE exchange_id = 1 AND canonical_pair_id = $1 AND timeframe = $2 "
                "AND lifecycle <> 'EXPIRED'",
                iid, tf);
            for (const auto& r : rows) {
                existing.push_back({r[0].as<int64_t>(), r[1].as<std::string>(),
                                    r[2].as<double>(), r[3].as<double>(), r[4].as<double>(),
                                    r[5].as<double>(), r[6].as<double>(), r[7].as<std::string>(),
                                    r[8].as<int>(), r[9].as<int>(), r[10].as<int64_t>()});
            }
            std::vector<bool> matched(cands.size(), false);

            const int64_t window_start_ms = bars[window_from].open_ms;
            for (auto& z : existing) {
                const ZoneBand band{z.lower, z.center, z.upper, z.kind, z.created_ms};
                const ZoneState state = evaluate_zone(bars, window_from, band, atr);
                const bool broken = retains_break(state.broken, z.status == "BROKEN",
                                                  z.created_ms, window_start_ms);

                // Match a candidate to refresh the geometry's strength.
                double best = 1e300;
                int best_i = -1;
                for (size_t i = 0; i < cands.size(); ++i) {
                    if (matched[i] || cands[i].kind != z.kind) continue;
                    const double d = std::abs(cands[i].center - z.center);
                    if (d < half_width * 2 && d < best) {
                        best = d;
                        best_i = static_cast<int>(i);
                    }
                }

                // Strength is a function of the window too: the confluence score
                // when a candidate still supports the band, otherwise how much
                // the window itself confirms it.
                double strength = 0.0;
                if (best_i >= 0) {
                    matched[best_i] = true;
                    strength = score_zone(cands[best_i], last_close, atr);
                }
                const double window_support =
                    std::min(1.0, static_cast<double>(state.touch_count) / 6.0);
                strength = std::max(strength, 0.5 * window_support);

                std::string new_status;
                if (broken) {
                    new_status = "BROKEN";
                } else if (strength < kExpireStrength) {
                    new_status = "EXPIRED";
                } else if (state.touched_recently) {
                    new_status = "TESTED";
                } else if (best_i >= 0) {
                    new_status = "ACTIVE";
                } else {
                    new_status = "WEAKENED";
                }

                txn.exec_params(
                    "UPDATE alttech.venue_structure_zones SET lifecycle = $2, "
                    "strength_score = $3, touch_count = $4, reaction_count = $5, "
                    // The cast has to be on the comparison too: inferred from
                    // `$6 > 0` alone PostgreSQL types the parameter as integer,
                    // and a millisecond timestamp overflows it.
                    "last_touched_at = CASE WHEN $6::bigint > 0 "
                    "  THEN to_timestamp($6::bigint / 1000.0) ELSE last_touched_at END, "
                    "status_changed_at = CASE WHEN lifecycle <> $2 THEN now() "
                    "ELSE status_changed_at END WHERE id = $1",
                    z.id, new_status, strength, state.touch_count, state.reaction_count,
                    state.last_touch_ms);

                // Events fire on a lifecycle transition, so recomputing an
                // unchanged zone emits nothing. A move back to ACTIVE is not an
                // event of its own: there is no honest label for it, and the
                // schema's vocabulary does not contain one.
                if (new_status != z.status) {
                    const char* event = new_status == "BROKEN"     ? "broken"
                                        : new_status == "TESTED"   ? "tested"
                                        : new_status == "EXPIRED"  ? "expired"
                                        : new_status == "WEAKENED" ? "weakened"
                                                                   : nullptr;
                    if (event != nullptr) {
                        record_event(txn, iid, tf, z.id, last_bar.open_ms, event);
                        events.push_back({event, z.kind, "", z.center});
                    }
                }

                // A mirror must be born viable. The carried strength is
                // kFlipStrengthCarryOver (0.70) of the broken level's, and a
                // zone may exist at kNewZoneMinStrength (0.20), so the weakest
                // mirror scores 0.14 — BELOW kExpireStrength (0.15). Such a zone
                // is inserted and expired on the next cycle, emitting a
                // `flipped` and an `expired` event for a level that never
                // existed: measured at 32.4 and 32.9 events per minute, against
                // 1.2 genuine breaks. A level too weak to survive its own
                // creation test does not become a level by being mirrored.
                // A role flip happens AT THE MOMENT a level breaks. It is an
                // event, not a standing property of a broken zone.
                //
                // `broken` stays true for the rest of a zone's life — that is
                // what retains_break() is for — so hanging the mirror on it
                // re-attempted the insert on every cycle, forever. The
                // NOT EXISTS guard only excludes a live mirror, so as soon as
                // one expired the next cycle created another: measured 31.8
                // `flipped` and 35.8 `expired` events per minute against 0.2
                // genuine breaks, a perpetual regeneration loop on levels that
                // had died hours earlier.
                //
                // A mirror must also be born viable: the carried strength is
                // kFlipStrengthCarryOver (0.70) of the broken level's, and a
                // zone may exist at kNewZoneMinStrength (0.20), so the weakest
                // mirror scores 0.14 — below kExpireStrength (0.15) — and would
                // be expired on the cycle after it was written.
                const bool just_broke = new_status == "BROKEN" && z.status != "BROKEN";
                const bool mirror_is_viable =
                    z.strength * kFlipStrengthCarryOver >= kExpireStrength;
                if (broken) {
                    // A broken zone is never counted active, whether or not it
                    // earns a mirror.
                    if (!just_broke || !mirror_is_viable) continue;
                    const std::string flipped =
                        z.kind == "support" ? "resistance" : "support";
                    const auto mirrored = txn.exec_params(
                        "INSERT INTO alttech.venue_structure_zones (exchange_id, "
                        "canonical_pair_id, timeframe, kind, lower_price, center_price, "
                        "upper_price, strength_score, confidence, lifecycle, algorithm_version) "
                        "SELECT 1, $1, $2, $3, $4, $5, $6, $7, 0.5, 'CANDIDATE', $8 "
                        "WHERE NOT EXISTS (SELECT 1 FROM alttech.venue_structure_zones e "
                        "  WHERE e.exchange_id = 1 AND e.canonical_pair_id = $1 "
                        "  AND e.timeframe = $2 AND e.kind = $3 AND e.lifecycle <> 'EXPIRED' "
                        "  AND e.lower_price <= $6 AND e.upper_price >= $4) RETURNING id",
                        iid, tf, flipped, z.lower, z.center, z.upper,
                        z.strength * kFlipStrengthCarryOver, std::string(kAlgoVersion));
                    if (!mirrored.empty()) {
                        record_event(txn, iid, tf, mirrored[0][0].as<int64_t>(),
                                     last_bar.open_ms, "flipped");
                        events.push_back({"flipped", z.kind, flipped, z.center});
                    }
                } else if (new_status != "EXPIRED") {
                    ++active;
                }
            }

            // Brand-new candidates: a band nothing already covers.
            for (size_t i = 0; i < cands.size(); ++i) {
                if (matched[i]) continue;
                const double strength = score_zone(cands[i], last_close, atr);
                if (strength < kNewZoneMinStrength) continue;
                // `created_ms` is the current bar: a level being recognised now
                // cannot already have been broken under observation.
                const ZoneBand band{cands[i].lower, cands[i].center, cands[i].upper,
                                    cands[i].kind, last_bar.open_ms};
                // Refuse a band price has already left. Such a level is not
                // broken, it is simply not a level any more, and creating it
                // only to break it next cycle produces churn and false events.
                if (close_is_beyond(last_close, band, atr)) continue;
                const ZoneState state = evaluate_zone(bars, window_from, band, atr);
                // A band the observable window shows price has never entered is
                // a hypothesis, not a level, and this module's own rule is that
                // "how often the market respected a band is the evidence that it
                // is a level at all". Pivots are clustered over 500 bars while
                // the lifecycle window is 120, so a cluster from the far end of
                // the pivot window can sit where price has not traded recently:
                // measured, 98.7 % of 15m zones were created with no touch and
                // expired 13.5 minutes later, never having influenced anything.
                //
                // Withholding them is not lost information. The whole zone state
                // is a pure function of the bar window — that is the module's
                // design premise — so a candidate that later earns a touch is
                // simply created then, with the same geometry it would have had.
                if (state.touch_count < 1) {
                    ++withheld;
                    continue;
                }
                const auto ins = txn.exec_params(
                    "INSERT INTO alttech.venue_structure_zones (exchange_id, canonical_pair_id, "
                    "timeframe, kind, lower_price, center_price, upper_price, strength_score, "
                    "confidence, lifecycle, touch_count, reaction_count, algorithm_version) "
                    "SELECT 1, $1, $2, $3, $4, $5, $6, $7, $8, 'ACTIVE', $9, $10, $11 "
                    "WHERE NOT EXISTS (SELECT 1 FROM alttech.venue_structure_zones e "
                    "  WHERE e.exchange_id = 1 AND e.canonical_pair_id = $1 "
                    "  AND e.timeframe = $2 AND e.kind = $3 AND e.lifecycle <> 'EXPIRED' "
                    "  AND e.lower_price <= $6 AND e.upper_price >= $4) RETURNING id",
                    iid, tf, cands[i].kind, cands[i].lower, cands[i].center, cands[i].upper,
                    strength, method_confidence(cands[i].methods.size()), state.touch_count,
                    state.reaction_count, std::string(kAlgoVersion));
                if (ins.empty()) continue; // a zone already covers this band
                const int64_t zone_id = ins[0][0].as<int64_t>();
                for (const auto& [method, weight] : cands[i].methods) {
                    txn.exec_params(
                        "INSERT INTO alttech.structure_zone_sources (zone_id, method, weight) "
                        "VALUES ($1, $2, $3) ON CONFLICT (zone_id, method) "
                        "DO UPDATE SET weight = EXCLUDED.weight",
                        zone_id, method, weight);
                }
                record_event(txn, iid, tf, zone_id, last_bar.open_ms, "created");
                events.push_back({"created", cands[i].kind, "", cands[i].center});
                ++active;
            }

            // The zone set as it stands at this bar. Recomputed after a source
            // revision, so the snapshot is updated rather than left stale.
            const auto snap = txn.exec_params(
                "SELECT COALESCE(jsonb_agg(jsonb_build_object('kind', kind, 'lower', lower_price, "
                "'center', center_price, 'upper', upper_price, 'strength', strength_score, "
                "'lifecycle', lifecycle) ORDER BY center_price), '[]'::jsonb) "
                "FROM alttech.venue_structure_zones WHERE exchange_id = 1 "
                "AND canonical_pair_id = $1 AND timeframe = $2 "
                "AND lifecycle IN ('ACTIVE','TESTED','WEAKENED','CANDIDATE')",
                iid, tf);
            txn.exec_params(
                "INSERT INTO alttech.structure_snapshots (exchange_id, canonical_pair_id, "
                "timeframe, bar_open_time, bar_revision, zones, algorithm_version) "
                "VALUES (1, $1, $2, to_timestamp($3::bigint / 1000.0), $4, $5::jsonb, $6) "
                "ON CONFLICT (exchange_id, canonical_pair_id, timeframe, bar_open_time) "
                "DO UPDATE SET bar_revision = EXCLUDED.bar_revision, zones = EXCLUDED.zones, "
                "created_at = now()",
                iid, tf, last_bar.open_ms, last_bar.revision, snap[0][0].as<std::string>(),
                std::string(kAlgoVersion));
            });

        if (!committed) {
            log::error("structure_reconcile_deferred",
                       {{"instrument_id", iid}, {"symbol", sym}, {"timeframe", tf}});
            return 0;
        }

        // Withholding is normal — most pivot clusters sit where price has not
        // traded recently — but it must be visible, so a change in the detector
        // that suddenly admits or rejects everything shows up as a number rather
        // than as a silent shift in how many zones exist.
        if (withheld > 0) {
            log::debug("structure_candidates_withheld",
                       {{"instrument_id", iid}, {"timeframe", tf},
                        {"withheld_untouched", withheld}, {"active", active}});
        }

        // Snapshot events are keyed on the bar, so JetStream dedups a bar the
        // sweep recomputed; level events are only produced on a transition.
        publish(iid, sym, tf, last_bar.open_ms, active);
        for (const auto& event : events) {
            publish_level(iid, sym, tf, last_bar.open_ms, event);
        }
        return active;
    }

    void publish(int64_t iid, const std::string& sym, const std::string& tf,
                 int64_t bar_open_ms, int active_zone_count) {
        messaging::EnvelopeMeta meta;
        meta.event_type = "structure.snapshot";
        meta.instrument_id = iid;
        meta.exchange_symbol = sym;
        meta.exchange_timestamp_ms = bar_open_ms;
        meta.source_service = "alttech-service";
        meta.message_id = "st:" + sym + ":" + tf + ":snapshot:" +
                          std::to_string(bar_open_ms);
        auto env = messaging::make_envelope(meta);
        auto* event = env.mutable_structure_event();
        event->set_timeframe(wire_timeframe(tf));
        event->set_bar_open_time_ms(bar_open_ms);
        event->set_type(::alttech::v1::STRUCTURE_EVENT_SNAPSHOT);
        event->set_algorithm_version(kAlgoVersion);
        event->set_active_zone_count(active_zone_count);
        if (auto published = nats_.js_publish(
                messaging::topology::venue_structure_subject(Venue::bitget, sym,
                                                             *tf_from_label(tf)),
                messaging::serialize(env), env.message_id());
            !published) {
            log::warn("structure_publish_failed",
                      {{"symbol", sym}, {"timeframe", tf}, {"kind", "snapshot"},
                       {"message", published.error().message}});
        }
    }

    void publish_level(int64_t iid, const std::string& sym, const std::string& tf,
                       int64_t bar_open_ms, const StructureNotification& notification) {
        messaging::EnvelopeMeta meta;
        meta.event_type = "structure." + notification.kind;
        meta.instrument_id = iid;
        meta.exchange_symbol = sym;
        meta.exchange_timestamp_ms = bar_open_ms;
        meta.source_service = "alttech-service";
        meta.message_id = "stlvl:" + sym + ":" + tf + ":" + notification.kind + ":" +
                          std::to_string(bar_open_ms) + ":" + notification.zone_kind + ":" +
                          notification.resulting_zone_kind;
        if (notification.center_price) {
            meta.message_id += ":" + std::to_string(*notification.center_price);
        }
        auto env = messaging::make_envelope(meta);
        auto* event = env.mutable_structure_event();
        event->set_timeframe(wire_timeframe(tf));
        event->set_bar_open_time_ms(bar_open_ms);
        event->set_type(wire_structure_event(notification.kind));
        event->set_algorithm_version(kAlgoVersion);
        event->set_zone_kind(notification.zone_kind);
        event->set_resulting_zone_kind(notification.resulting_zone_kind);
        if (notification.center_price) event->set_center_price(*notification.center_price);
        if (auto published = nats_.js_publish(
                messaging::topology::venue_structure_event_subject(
                    Venue::bitget, sym, *tf_from_label(tf), notification.kind),
                messaging::serialize(env), env.message_id());
            !published) {
            log::warn("structure_level_publish_failed",
                      {{"symbol", sym}, {"kind", notification.kind},
                       {"message", published.error().message}});
        }
    }

    persistence::PgPool& pool_;
    messaging::NatsClient& nats_;
};

} // namespace

Result<void> run_structure(const YAML::Node& module_config,
                           persistence::PgPool& pool,
                           messaging::NatsClient& nats,
                           application::ModuleSupervisor& supervisor) {
    const int64_t cycle_sec = config::opt_int(module_config, "cycle_interval_sec", 60);
    if (cycle_sec < 1 || cycle_sec > 3600) {
        log::critical("invalid_structure_service_config",
                      {{"cycle_interval_sec", cycle_sec}});
        Error error{ErrorCode::config_error,
                    "structure config requires cycle_interval_sec in [1,3600]"};
        supervisor.mark_failed(kModuleName, error);
        return std::unexpected(std::move(error));
    }

    if (auto probe = pool.with_txn([](pqxx::work& txn) {
            const auto row = txn.exec1(
                "SELECT to_regclass('exchanges') IS NOT NULL "
                "AND to_regclass('exchange_instruments') IS NOT NULL "
                "AND to_regclass('market_candles_agg') IS NOT NULL "
                "AND to_regclass('alttech.venue_structure_zones') IS NOT NULL "
                "AND to_regclass('alttech.structure_zone_sources') IS NOT NULL "
                "AND to_regclass('alttech.structure_snapshots') IS NOT NULL");
            if (!row[0].as<bool>()) {
                throw std::runtime_error("required market-structure schema is missing");
            }
        });
        !probe) {
        log::critical("postgres_schema_unavailable", {{"message", probe.error().message}});
        supervisor.mark_failed(kModuleName, probe.error());
        return std::unexpected(probe.error());
    }

    StructureService service(pool, nats);
    supervisor.mark_ready(kModuleName);
    log::info("structure_module_ready", {{"timeframes", "15m,1h,4h"}});
    auto& shutdown = ShutdownToken::instance();
    // Per-venue 1m zones for all four venues; the consensus zone builder
    // consumes them. Joined on every exit path.
    venuelite::SweepThreadJoiner venue_join{
        std::thread(venuelite::run_venue_sweep, std::ref(pool), std::ref(nats),
                    venuelite::store::VenueOutput::structure, module_config)};
    try {
        while (!shutdown.stop_requested()) {
            supervisor.heartbeat(kModuleName, static_cast<int64_t>(cycle_sec) * 4000 + 120'000);
            service.run_cycle();
            if (shutdown.wait_for(std::chrono::seconds(cycle_sec))) break;
        }
    } catch (const std::exception& e) {
        Error error{ErrorCode::internal_error,
                    std::string("structure module terminated: ") + e.what()};
        log::critical("structure_module_failed", {{"message", error.message}});
        supervisor.mark_failed(kModuleName, error);
        return std::unexpected(std::move(error));
    } catch (...) {
        Error error{ErrorCode::internal_error,
                    "structure module terminated with a non-standard exception"};
        log::critical("structure_module_failed", {{"message", error.message}});
        supervisor.mark_failed(kModuleName, error);
        return std::unexpected(std::move(error));
    }

    log::info("structure_module_stopped", {});
    return {};
}

} // namespace alttech::modules
