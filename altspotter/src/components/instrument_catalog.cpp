// instrument_catalog: multi-venue instrument catalogue + global universe resolver.
//
// Syncs the USDT-linear-perpetual contract of each configured base asset on all
// FOUR venues (Bitget, Binance USDS-M, Bybit linear, OKX USDT swap), maintains
// the canonical pair identity + per-venue mapping and spec-version history, and
// computes the GLOBAL OBSERVATION UNIVERSE:
//
//   eligible(pair) = present AND active on every enabled venue.
//
// A pair missing/inactive on even one venue is globally excluded: is_whitelisted
// is cleared on every venue's exchange_instruments, so no runtime subscribes,
// backfills or stores it anywhere. Downstream components read
// `is_whitelisted AND status='active'`, so they inherit the intersection for free.
//
// Transient venue outage != delisting: if any enabled venue's contract list
// fails this cycle, eligibility is NOT recomputed (previous whitelist preserved,
// universe marked TEMPORARILY_UNVERIFIED) — one network blip never mass-drops
// pairs.

#include "components/components.hpp"
#include "core/hash.hpp"
#include "core/readiness.hpp"
#include "core/shutdown.hpp"
#include "config/config.hpp"
#include "exchanges/binance/binance_adapter.hpp"
#include "exchanges/bybit/bybit_adapter.hpp"
#include "exchanges/okx/okx_adapter.hpp"
#include "exchanges/bitget/bitget_adapter.hpp"
#include "exchanges/venues.hpp"
#include "logging/logger.hpp"
#include "messaging/codec.hpp"
#include "messaging/nats_client.hpp"
#include "messaging/subjects.hpp"
#include "persistence/pg.hpp"
#include "core/time.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

using namespace altspotter;

namespace {

struct VenueAdapter {
    int16_t exchange_id;
    std::string code;
    std::unique_ptr<exchanges::ExchangeMarketDataAdapter> adapter;
};

std::optional<VenueAdapter> make_venue_adapter(int16_t id, const YAML::Node& node,
                                               std::function<bool()> stop) {
    if (!node || !node.IsMap()) return std::nullopt;
    VenueAdapter va;
    va.exchange_id = id;
    switch (id) {
        case exchanges::kVenueBitget: {
            auto cfg = config::parse_bitget_rest(node);
            if (!cfg) return std::nullopt;
            va.code = "bitget";
            va.adapter = std::make_unique<exchanges::bitget::BitgetMarketDataAdapter>(*cfg, stop);
            break;
        }
        case exchanges::kVenueBinanceUsdm:
            va.code = "binance_usdm";
            va.adapter = std::make_unique<exchanges::binance::BinanceUsdmMarketDataAdapter>(
                exchanges::binance::parse_config(node), stop);
            break;
        case exchanges::kVenueBybit:
            va.code = "bybit";
            va.adapter = std::make_unique<exchanges::bybit::BybitLinearMarketDataAdapter>(
                exchanges::bybit::parse_config(node), stop);
            break;
        case exchanges::kVenueOkx:
            va.code = "okx";
            va.adapter = std::make_unique<exchanges::okx::OkxUsdtSwapMarketDataAdapter>(
                exchanges::okx::parse_config(node), stop);
            break;
        default:
            return std::nullopt;
    }
    return va;
}

Result<std::vector<std::string>> load_whitelist_bases(const std::string& path) {
    auto doc = config::load_yaml(path);
    if (!doc) return std::unexpected(doc.error());
    const auto assets = (*doc)["assets"];
    if (!assets || !assets.IsSequence()) {
        return fail(ErrorCode::config_error, "symbols file has no 'assets' sequence");
    }
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto& a : assets) {
        const std::string base = a["base"].as<std::string>("");
        if (base.empty()) return fail(ErrorCode::config_error, "whitelist entry has empty base");
        if (!seen.insert(base).second) {
            return fail(ErrorCode::config_error, "duplicate base in whitelist: " + base);
        }
        out.push_back(base);
    }
    if (out.empty() || out.size() > 30) {
        return fail(ErrorCode::config_error,
                    "whitelist must contain 1..30 bases, found " + std::to_string(out.size()));
    }
    return out;
}

nlohmann::json spec_to_json(const InstrumentSpec& s) {
    return {
        {"exchange_symbol", s.exchange_symbol}, {"base_asset", s.base_asset},
        {"quote_asset", s.quote_asset}, {"settle_asset", s.settle_asset},
        {"contract_type", s.contract_type}, {"contract_value", s.contract_value.to_string()},
        {"status", std::string(instrument_status_label(s.status))},
        {"list_time_ms", s.list_time_ms}, {"tick_size", s.tick_size.to_string()},
        {"price_precision", s.price_precision}, {"quantity_step", s.quantity_step.to_string()},
        {"quantity_precision", s.quantity_precision},
        {"min_order_quantity", s.min_order_quantity.to_string()},
        {"min_notional", s.min_notional.to_string()}, {"max_leverage", s.max_leverage},
    };
}

// Per-venue view of one base asset this cycle.
struct VenuePair {
    bool queried_ok = false; // venue contract list fetched successfully
    bool listed = false;
    bool active = false;
    InstrumentSpec spec;
};

struct PairState {
    std::string base;
    std::map<int16_t, VenuePair> venues; // by exchange_id
    bool eligible = false;
    std::string status;
    std::string reason;
};

// (subject, envelope) events to publish after the transaction commits.
struct OutEvent {
    std::string subject;
    altspotter::v1::Envelope env;
};

class MultiVenueCatalog {
public:
    MultiVenueCatalog(config::ServiceContext ctx, std::vector<std::string> bases,
                      std::vector<VenueAdapter> venues, std::unique_ptr<messaging::NatsClient> nats,
                      persistence::PgPool& pool)
        : ctx_(std::move(ctx)), bases_(std::move(bases)), venues_(std::move(venues)),
          nats_(std::move(nats)), pool_(pool) {}

    void sync_once() {
        // 1. Pull every enabled venue's contract list (base -> spec).
        std::map<int16_t, std::map<std::string, InstrumentSpec>> by_venue;
        bool all_ok = true;
        for (auto& v : venues_) {
            auto contracts = v.adapter->list_instruments();
            if (!contracts) {
                all_ok = false;
                log::warn("catalog_venue_list_failed",
                          {{"venue", v.code}, {"message", contracts.error().message}});
                continue;
            }
            auto& m = by_venue[v.exchange_id];
            for (auto& s : *contracts) {
                // Keep the first (active preferred) contract per base asset.
                auto it = m.find(s.base_asset);
                if (it == m.end() || (s.status == InstrumentStatus::active &&
                                      it->second.status != InstrumentStatus::active)) {
                    m[s.base_asset] = std::move(s);
                }
            }
        }

        // 2. Build per-pair four-venue state + eligibility.
        std::vector<PairState> pairs;
        pairs.reserve(bases_.size());
        for (const auto& base : bases_) {
            PairState ps;
            ps.base = base;
            bool eligible = true;
            std::string reason;
            for (const auto& v : venues_) {
                VenuePair vp;
                const auto vit = by_venue.find(v.exchange_id);
                vp.queried_ok = vit != by_venue.end();
                if (vp.queried_ok) {
                    const auto sit = vit->second.find(base);
                    if (sit != vit->second.end()) {
                        vp.listed = true;
                        vp.spec = sit->second;
                        vp.active = sit->second.status == InstrumentStatus::active;
                    }
                }
                if (!vp.listed) {
                    eligible = false;
                    if (reason.empty()) reason = "missing on " + v.code;
                    if (ps.status.empty()) ps.status = venue_missing_status(v.exchange_id);
                } else if (!vp.active) {
                    eligible = false;
                    if (reason.empty()) reason = "inactive on " + v.code;
                    if (ps.status.empty()) ps.status = "INACTIVE_ON_VENUE";
                }
                ps.venues[v.exchange_id] = std::move(vp);
            }
            ps.eligible = eligible;
            if (eligible) {
                ps.status = "ELIGIBLE_ALL_VENUES";
                ps.reason = "present and active on all venues";
            } else {
                ps.reason = reason;
            }
            pairs.push_back(std::move(ps));
        }

        // 3. Persist. When a venue failed this cycle, keep is_whitelisted /
        //    eligibility unchanged (transient outage, not a delisting).
        std::vector<OutEvent> events;
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            txn.exec("SET LOCAL TIME ZONE 'UTC'");
            for (auto& ps : pairs) persist_pair(txn, ps, all_ok, events);
        });
        if (!res) {
            log::error("catalog_sync_failed", {{"message", res.error().message}});
            return;
        }

        for (auto& e : events) {
            const auto bytes = messaging::serialize(e.env);
            if (auto r = nats_->js_publish(e.subject, bytes, e.env.message_id()); !r) {
                log::warn("catalog_event_publish_failed",
                          {{"subject", e.subject}, {"message", r.error().message}});
            }
        }
        publish_universe_snapshot(pairs);

        int eligible_count = 0;
        for (const auto& ps : pairs) if (ps.eligible) ++eligible_count;
        log::info("catalog_sync_done", {{"bases", pairs.size()},
                                        {"eligible", eligible_count},
                                        {"all_venues_ok", all_ok},
                                        {"events", events.size()}});
    }

private:
    static const char* venue_missing_status(int16_t id) {
        switch (id) {
            case exchanges::kVenueBitget: return "MISSING_ON_BITGET";
            case exchanges::kVenueBinanceUsdm: return "MISSING_ON_BINANCE";
            case exchanges::kVenueBybit: return "MISSING_ON_BYBIT";
            case exchanges::kVenueOkx: return "MISSING_ON_OKX";
            default: return "EXCLUDED";
        }
    }

    void persist_pair(pqxx::work& txn, PairState& ps, bool all_ok, std::vector<OutEvent>& events) {
        // Canonical instrument + pair (id shared across venues).
        const auto instr = txn.exec_params(
            "INSERT INTO instruments (base_asset, quote_asset, instrument_type) "
            "VALUES ($1, 'USDT', 'perpetual') "
            "ON CONFLICT (base_asset, quote_asset, instrument_type) "
            "DO UPDATE SET base_asset = EXCLUDED.base_asset RETURNING id",
            ps.base);
        const int64_t pair_id = instr[0][0].as<int64_t>();
        txn.exec_params(
            "INSERT INTO canonical_pairs (id, base_asset, quote_asset) VALUES ($1, $2, 'USDT') "
            "ON CONFLICT (id) DO NOTHING",
            pair_id, ps.base);

        // Per-venue exchange_instruments + spec versions + mapping + events.
        for (auto& [ex_id, vp] : ps.venues) {
            if (!vp.queried_ok) continue; // outage: leave this venue's row untouched
            if (!vp.listed) {
                // Absent this cycle: mark not_listed if a row exists.
                txn.exec_params(
                    "UPDATE exchange_instruments SET status = 'not_listed' "
                    "WHERE exchange_id = $1 AND instrument_id = $2 AND status <> 'not_listed'",
                    ex_id, pair_id);
                continue;
            }
            persist_venue_instrument(txn, ex_id, pair_id, vp.spec, events);
        }

        // is_whitelisted across all venues of the pair follows eligibility, but
        // only when every venue was verified this cycle.
        if (all_ok) {
            txn.exec_params(
                "UPDATE exchange_instruments SET is_whitelisted = $2 WHERE instrument_id = $1",
                pair_id, ps.eligible);
        }

        // Universe row + transition event.
        std::string prev_status;
        const auto prev = txn.exec_params(
            "SELECT status FROM global_observation_universe WHERE canonical_pair_id = $1", pair_id);
        if (!prev.empty()) prev_status = prev[0][0].as<std::string>();

        const std::string status = all_ok ? ps.status : (prev_status.empty() ? "TEMPORARILY_UNVERIFIED"
                                                                              : prev_status);
        const bool eligible_now = all_ok ? ps.eligible : (prev_status == "ELIGIBLE_ALL_VENUES");
        auto venue_flag = [&](int16_t id, bool active) -> bool {
            const auto it = ps.venues.find(id);
            if (it == ps.venues.end() || !it->second.queried_ok) return false;
            return active ? it->second.active : it->second.listed;
        };
        txn.exec_params(
            "INSERT INTO global_observation_universe (canonical_pair_id, status, eligible, reason, "
            "bitget_listed, binance_listed, okx_listed, bybit_listed, "
            "bitget_active, binance_active, okx_active, bybit_active, "
            "first_eligible_at, last_evaluated_at, updated_at) "
            "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12, "
            "CASE WHEN $3 THEN now() END, now(), now()) "
            "ON CONFLICT (canonical_pair_id) DO UPDATE SET status = $2, eligible = $3, reason = $4, "
            "bitget_listed = $5, binance_listed = $6, okx_listed = $7, bybit_listed = $8, "
            "bitget_active = $9, binance_active = $10, okx_active = $11, bybit_active = $12, "
            "first_eligible_at = COALESCE(global_observation_universe.first_eligible_at, "
            "CASE WHEN $3 THEN now() END), last_evaluated_at = now(), updated_at = now()",
            pair_id, status, eligible_now, ps.reason,
            venue_flag(exchanges::kVenueBitget, false), venue_flag(exchanges::kVenueBinanceUsdm, false),
            venue_flag(exchanges::kVenueOkx, false), venue_flag(exchanges::kVenueBybit, false),
            venue_flag(exchanges::kVenueBitget, true), venue_flag(exchanges::kVenueBinanceUsdm, true),
            venue_flag(exchanges::kVenueOkx, true), venue_flag(exchanges::kVenueBybit, true));

        if (all_ok && status != prev_status) {
            const char* ev = status == "ELIGIBLE_ALL_VENUES" ? "eligible"
                             : prev_status == "ELIGIBLE_ALL_VENUES" ? "excluded" : "changed";
            txn.exec_params(
                "INSERT INTO global_universe_events (canonical_pair_id, event, previous_status, "
                "status, reason) VALUES ($1, $2, $3, $4, $5)",
                pair_id, ev, prev_status.empty() ? std::optional<std::string>() : prev_status,
                status, ps.reason);
            events.push_back(make_universe_change(pair_id, ps, prev_status));
        }
    }

    void persist_venue_instrument(pqxx::work& txn, int16_t ex_id, int64_t pair_id,
                                  const InstrumentSpec& spec, std::vector<OutEvent>& events) {
        const std::string new_status(instrument_status_label(spec.status));
        const auto prev = txn.exec_params(
            "SELECT id, status FROM exchange_instruments WHERE exchange_id = $1 AND instrument_id = $2",
            ex_id, pair_id);
        int64_t ei_id = 0;
        std::string prev_status = "missing";
        if (prev.empty()) {
            const auto ins = txn.exec_params(
                "INSERT INTO exchange_instruments (exchange_id, instrument_id, exchange_symbol, "
                "status, is_whitelisted, list_time, last_seen_at, status_changed_at, status_reason) "
                "VALUES ($1,$2,$3,$4,false, "
                "CASE WHEN $5::bigint > 0 THEN to_timestamp($5::bigint/1000.0) END, now(), now(), "
                "'discovered') RETURNING id",
                ex_id, pair_id, spec.exchange_symbol, new_status, spec.list_time_ms);
            ei_id = ins[0][0].as<int64_t>();
        } else {
            ei_id = prev[0][0].as<int64_t>();
            prev_status = prev[0][1].as<std::string>();
            txn.exec_params(
                "UPDATE exchange_instruments SET exchange_symbol = $2, status = $3, "
                "list_time = CASE WHEN $4::bigint > 0 THEN to_timestamp($4::bigint/1000.0) ELSE list_time END, "
                "last_seen_at = now(), "
                "status_changed_at = CASE WHEN status <> $3 THEN now() ELSE status_changed_at END "
                "WHERE id = $1",
                ei_id, spec.exchange_symbol, new_status, spec.list_time_ms);
        }
        txn.exec_params(
            "INSERT INTO canonical_pair_exchange_map (canonical_pair_id, exchange_id, "
            "exchange_instrument_id) VALUES ($1, $2, $3) "
            "ON CONFLICT (canonical_pair_id, exchange_id) DO UPDATE SET exchange_instrument_id = $3",
            pair_id, ex_id, ei_id);

        // Spec version on hash change.
        const std::string hash = sha256_hex(spec.normalized_fingerprint());
        const auto last_ver = txn.exec_params(
            "SELECT version, normalized_hash FROM instrument_spec_versions "
            "WHERE exchange_instrument_id = $1 ORDER BY version DESC LIMIT 1", ei_id);
        int64_t version = 1;
        bool new_version = last_ver.empty();
        if (!last_ver.empty()) {
            if (last_ver[0][1].as<std::string>() != hash) {
                version = last_ver[0][0].as<int64_t>() + 1;
                new_version = true;
            } else {
                version = last_ver[0][0].as<int64_t>();
            }
        }
        if (new_version) {
            txn.exec_params(
                "INSERT INTO instrument_spec_versions (exchange_instrument_id, version, "
                "normalized_hash, spec) VALUES ($1, $2, $3, $4)",
                ei_id, version, hash, spec_to_json(spec).dump());
        }

        // Emit instrument events (created / updated / enabled / disabled).
        const auto emit = [&](altspotter::v1::InstrumentEventType type, const char* kind,
                              const std::string& reason) {
            messaging::EnvelopeMeta meta;
            meta.exchange_id = ex_id;
            meta.instrument_id = pair_id;
            meta.exchange_symbol = spec.exchange_symbol;
            meta.source_service = "instrument_catalog";
            meta.message_id = std::string("instr:") + venue_code(ex_id) + ":" + kind + ":" +
                              spec.exchange_symbol + ":" + std::to_string(version) + ":" +
                              hash.substr(0, 12);
            meta.event_type = std::string("instrument.") + kind;
            auto env = messaging::make_envelope(meta);
            auto* ev = env.mutable_instrument_event();
            ev->set_type(type);
            ev->set_reason(reason);
            messaging::to_proto(spec, pair_id, hash, version, ev->mutable_spec(), ex_id);
            events.push_back({subjects::instrument_event(venue_code(ex_id), kind), std::move(env)});
        };
        const bool was_active = prev_status == "active";
        const bool is_active = spec.status == InstrumentStatus::active;
        if (prev.empty()) emit(altspotter::v1::INSTRUMENT_EVENT_CREATED, "created", "first sync");
        else if (new_version) emit(altspotter::v1::INSTRUMENT_EVENT_UPDATED, "updated", "spec changed");
        if (is_active && !was_active) emit(altspotter::v1::INSTRUMENT_EVENT_ENABLED, "enabled", "active");
        else if (!is_active && was_active) {
            emit(altspotter::v1::INSTRUMENT_EVENT_DISABLED, "disabled", new_status);
        }
    }

    std::string venue_code(int16_t id) const {
        return std::string(exchanges::venue_code(id));
    }

    OutEvent make_universe_change(int64_t pair_id, const PairState& ps,
                                  const std::string& prev_status) {
        messaging::EnvelopeMeta meta;
        meta.instrument_id = pair_id;
        meta.source_service = "instrument_catalog";
        meta.event_type = "universe.change";
        meta.message_id = "univ:change:" + std::to_string(pair_id) + ":" +
                          std::to_string(timeutil::now_ms());
        auto env = messaging::make_envelope(meta);
        auto* uc = env.mutable_universe_change();
        uc->set_previous_status(prev_status);
        fill_pair_state(uc->mutable_pair(), pair_id, ps);
        const char* kind = ps.eligible ? "pair.eligible" : "pair.excluded";
        return {subjects::universe_event(kind), std::move(env)};
    }

    void fill_pair_state(altspotter::v1::UniversePairState* out, int64_t pair_id, const PairState& ps) {
        out->set_canonical_pair_id(pair_id);
        out->set_base_asset(ps.base);
        out->set_quote_asset("USDT");
        out->set_eligible(ps.eligible);
        out->set_status(ps.status);
        out->set_reason(ps.reason);
        for (const auto& v : venues_) {
            auto* vs = out->add_venues();
            vs->set_exchange(static_cast<altspotter::v1::Exchange>(v.exchange_id));
            const auto it = ps.venues.find(v.exchange_id);
            if (it != ps.venues.end()) {
                vs->set_listed(it->second.listed);
                vs->set_active(it->second.active);
                vs->set_exchange_symbol(it->second.spec.exchange_symbol);
            }
        }
    }

    void publish_universe_snapshot(const std::vector<PairState>& pairs) {
        messaging::EnvelopeMeta meta;
        meta.source_service = "instrument_catalog";
        meta.event_type = "universe.snapshot";
        meta.message_id = "univ:snapshot:" + std::to_string(timeutil::now_ms());
        auto env = messaging::make_envelope(meta);
        auto* snap = env.mutable_universe_snapshot();
        snap->set_computed_at_ms(timeutil::now_ms());
        for (const auto& ps : pairs) {
            fill_pair_state(snap->add_pairs(), pair_id_of(ps), ps);
        }
        const auto bytes = messaging::serialize(env);
        if (auto r = nats_->js_publish(subjects::universe_event("snapshot"), bytes,
                                       env.message_id());
            !r) {
            log::warn("universe_snapshot_publish_failed", {{"message", r.error().message}});
        }
    }

    int64_t pair_id_of(const PairState& ps) {
        auto it = pair_ids_.find(ps.base);
        if (it != pair_ids_.end()) return it->second;
        int64_t id = 0;
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            const auto r = txn.exec_params(
                "SELECT id FROM instruments WHERE base_asset = $1 AND quote_asset = 'USDT' "
                "AND instrument_type = 'perpetual'",
                ps.base);
            if (!r.empty()) id = r[0][0].as<int64_t>();
        });
        // A failed lookup must not be cached. The transaction result used to be
        // discarded with `(void)res`, so a transient database error left `id`
        // at 0, that 0 was written into the cache, and every later call
        // returned it without ever retrying. The value goes out on the wire as
        // `canonical_pair_id` in the universe snapshot, so one blip produced a
        // permanently invalid identifier for that asset until a restart.
        //
        // A genuine "not found" is also not cached: an instrument that has not
        // been catalogued yet appears later, and caching 0 would hide it.
        if (!res) {
            log::warn("pair_id_lookup_failed",
                      {{"base_asset", ps.base}, {"message", res.error().message}});
            return 0;
        }
        if (id <= 0) return 0;
        pair_ids_[ps.base] = id;
        return id;
    }

    config::ServiceContext ctx_;
    std::vector<std::string> bases_;
    std::vector<VenueAdapter> venues_;
    std::unique_ptr<messaging::NatsClient> nats_;
    persistence::PgPool& pool_;
    std::map<std::string, int64_t> pair_ids_;
};

// Resolve each venue's REST config node from the unified config.
YAML::Node venue_rest_node(const config::ServiceContext& ctx, int16_t id) {
    switch (id) {
        case exchanges::kVenueBitget: return ctx.service["bitget"];
        case exchanges::kVenueBinanceUsdm: return ctx.root["binance_usdm_runtime"];
        case exchanges::kVenueBybit: return ctx.root["bybit_runtime"];
        case exchanges::kVenueOkx: return ctx.root["okx_runtime"];
        default: return YAML::Node();
    }
}

bool venue_enabled(const config::ServiceContext& ctx, const char* code) {
    const auto ex = ctx.root["exchanges"];
    if (!ex || !ex[code]) return true;
    return config::opt_bool(ex[code], "enabled", true);
}

} // namespace

namespace altspotter::components {

int instrument_catalog(config::ServiceContext ctx) {
    const std::string symbols_file =
        config::opt_str(ctx.service, "symbols_file", "config/symbols.yaml");
    const int64_t sync_interval_sec = config::opt_int(ctx.service, "sync_interval_sec", 900);

    auto bases = load_whitelist_bases(symbols_file);
    if (!bases) {
        log::critical("bad_whitelist", {{"message", bases.error().message}});
        return 2;
    }

    auto& shutdown = ShutdownToken::instance();
    auto stop_fn = [&shutdown] { return shutdown.stop_requested(); };

    std::vector<VenueAdapter> venues;
    for (const auto& v : exchanges::kVenues) {
        if (!venue_enabled(ctx, std::string(v.code).c_str())) {
            log::info("catalog_venue_disabled", {{"venue", std::string(v.code)}});
            continue;
        }
        auto va = make_venue_adapter(v.exchange_id, venue_rest_node(ctx, v.exchange_id), stop_fn);
        if (!va) {
            log::critical("catalog_venue_config_missing", {{"venue", std::string(v.code)}});
            return 2;
        }
        venues.push_back(std::move(*va));
    }
    if (venues.empty()) {
        log::critical("catalog_no_venues", {{"message", "no exchanges enabled"}});
        return 2;
    }

    auto nats = messaging::NatsClient::connect(ctx.common.nats_url, ctx.service_name);
    if (!nats) {
        log::critical("nats_unavailable", {{"message", nats.error().message}});
        return 1;
    }
    persistence::PgPool pool(ctx.common.postgres_dsn,
                             static_cast<size_t>(config::opt_int(ctx.service, "postgres_pool_size", 2)));
    if (auto probe = pool.with_txn([](pqxx::work& txn) { txn.exec1("SELECT 1"); }); !probe) {
        log::critical("postgres_unavailable", {{"message", probe.error().message}});
        return 1;
    }

    MultiVenueCatalog catalog(ctx, std::move(*bases), std::move(venues), std::move(*nats), pool);

    while (!shutdown.stop_requested()) {
        const auto started = timeutil::now_ms();
        catalog.sync_once();
        const auto elapsed = timeutil::now_ms() - started;
        const auto sleep_ms = std::max<int64_t>(1000, sync_interval_sec * 1000 - elapsed);
        if (shutdown.wait_for(std::chrono::milliseconds(sleep_ms))) break;
    }

    log::info("service_stopped", {});
    return 0;
}

}  // namespace altspotter::components
