// Feature module: historical backfill, live calculation and revision-driven
// recompute of the full TA-Lib feature set for the primary venue (Bitget)
// across 1m/5m/15m/30m/1h/4h/1d.
//
//   BACKFILL  : per shard (instrument x timeframe), read candles from
//               PostgreSQL in chunks, run the engine, batch-UPSERT vectors;
//               never loads a whole instrument into RAM.
//   LIVE      : durable JetStream consumer on bar.>; in-order closed bars
//               advance the shard engine and publish the vector; gaps or
//               out-of-order deliveries schedule a DB-driven catch-up.
//   RECOMPUTE : bar revisions reset the shard from the earliest revised bar
//               and rewrite the affected vectors.
//
// Restart recovery replays the trailing warm-up window from PostgreSQL; the
// checkpoint table tracks the last processed bar per shard. The same module
// also drives the reference-venue feature sweep, which produces the lighter
// per-venue vectors for Binance, Bybit and OKX.

#include "feature_store.hpp"
#include "service_logic.hpp"
#include "venue_sweep.hpp"

#include "alttech/persistence/maintenance_lock.hpp"
#include "alttech/application/module_supervisor.hpp"
#include "alttech/modules/feature.hpp"
#include "alttech/common/shutdown.hpp"
#include "alttech/config/config.hpp"
#include "alttech/logging/logger.hpp"
#include "alttech/messaging/codec.hpp"
#include "alttech/messaging/nats_client.hpp"
#include "alttech/messaging/topology.hpp"
#include "alttech/persistence/pg.hpp"
#include "alttech/talib_features/feature_engine.hpp"
#include "alttech/time/time.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cmath>
#include <deque>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>

using namespace alttech;
using namespace alttech::featsvc;

namespace {

constexpr std::string_view kModuleName = "feature";

struct Shard {
    Venue venue = Venue::bitget;   // which venue's candles this shard reads
    int64_t instrument_id = 0;
    std::string symbol;
    Timeframe tf = Timeframe::m1;
    std::string tf_label;
    talib_features::FeatureEngine engine;
    int64_t last_processed_ms = -1; // last bar fed into the engine (written)
    bool busy = false;              // a catch-up/recompute task owns it
    bool live_ready = false;        // status event bookkeeping
    std::optional<VectorRow> last_row;
    int64_t last_published_ms = -1;
    int64_t last_published_revision = -1;

    // Oldest stale-WARMING bar this shard last rewrote. A vector that stays
    // WARMING for a real reason (too few bars behind it, or a gap) would
    // otherwise be selected, rewritten to the same status, and selected again
    // forever. Remembering the last attempt makes the sweep converge: the same
    // bar is never chased twice.
    int64_t last_stale_from_ms = -1;
};

// (venue, instrument_id, tf minutes). The venue is part of the key because the
// canonical pair id is SHARED across venues: without it a Binance bar and a
// Bitget bar for the same pair collide on one shard. That collision is why this
// module used to drop every non-primary bar on arrival; keying on the venue is
// what lets all four be computed side by side.
using ShardKey = std::tuple<int32_t, int64_t, int32_t>;

struct Task : PendingTask {
    ShardKey key;
    std::chrono::steady_clock::time_point ready_at{};
};

// `divergent` is terminal: the event disagrees with the authoritative store in
// a way no amount of waiting resolves, so it is recorded and dropped rather than
// left to occupy the consumer's ack-pending budget forever.
// Inverse of to_proto_exchange(). An envelope whose venue we do not recognise is
// not computed: returning nullopt makes the caller acknowledge and move on
// rather than guess a venue and attribute the bar to the wrong one.
[[nodiscard]] std::optional<Venue> venue_from_proto(::alttech::v1::Exchange e) {
    switch (e) {
        case ::alttech::v1::EXCHANGE_BITGET: return Venue::bitget;
        case ::alttech::v1::EXCHANGE_BINANCE: return Venue::binance_usdm;
        case ::alttech::v1::EXCHANGE_OKX: return Venue::okx;
        case ::alttech::v1::EXCHANGE_BYBIT: return Venue::bybit;
        default: return std::nullopt;
    }
}

enum class Delivery { ack, retry, divergent };

struct TaskOutcome {
    bool ok = false;
    int64_t vectors_written = 0;
    std::string error;
};

class FeatureService {
public:
    FeatureService(const YAML::Node& service_config, persistence::PgPool& pool,
                   messaging::NatsClient& nats, int64_t feature_set_id)
        : pool_(pool), nats_(nats), feature_set_id_(feature_set_id) {
        publish_live_ = config::opt_bool(service_config, "publish_core_vectors", true);
        chunk_ = static_cast<int>(std::clamp<int64_t>(
            config::opt_int(service_config, "backfill_chunk", 5000), 1, 100000));
        write_batch_ = static_cast<size_t>(std::clamp<int64_t>(
            config::opt_int(service_config, "write_batch", 400), 1, 2000));
        retry_min_ms_ = std::clamp<int64_t>(
            config::opt_int(service_config, "task_retry_backoff_min_ms", 1000), 100, 60000);
        retry_max_ms_ = std::clamp<int64_t>(
            config::opt_int(service_config, "task_retry_backoff_max_ms", 30000),
            retry_min_ms_, 300000);
        // How long a cross-consumer storage barrier may wait before the
        // disagreement is treated as divergence rather than lag. Configurable
        // because the right bound depends on how far the storage writer can
        // legitimately fall behind on a given deployment; 0 restores the
        // unbounded wait.
        max_storage_wait_deliveries_ = std::clamp<int64_t>(
            config::opt_int(service_config, "max_storage_wait_deliveries", 20), 0, 100000);
        // Storage guard: analytics may only be written where a partition
        // exists, so the replay horizon is bounded by the retention window.
        backfill_days_ = std::clamp<int64_t>(
            config::opt_int(service_config, "backfill_days", 2), 1, 365);
        const std::string tfs = config::opt_str(service_config, "timeframes", "");
        for (size_t b = 0, e = 0; b < tfs.size(); b = e + 1) {
            e = tfs.find(',', b);
            if (e == std::string::npos) e = tfs.size();
            std::string t = tfs.substr(b, e - b);
            while (!t.empty() && (t.front() == ' ')) t.erase(t.begin());
            while (!t.empty() && (t.back() == ' ')) t.pop_back();
            if (!t.empty()) enabled_tfs_.insert(t);
        }
        const auto& names = talib_features::core_feature_names();
        for (size_t i = 0; i < names.size(); ++i) name_index_[names[i]] = i;
    }

    // Storage guards, read from config at start-up.
    std::set<std::string> enabled_tfs_;
    int64_t backfill_days_ = 2;

    Result<void> init_shards() {
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
        if (!res) return std::unexpected(res.error());

        static const Timeframe kTfs[] = {Timeframe::m1,  Timeframe::m5,  Timeframe::m15,
                                         Timeframe::m30, Timeframe::h1,  Timeframe::h4,
                                         Timeframe::d1};
        {
            std::lock_guard lock(mu_);
            std::vector<ShardKey> startup;
            for (const auto& [iid, sym] : instruments) {
                for (const Venue venue : kAllVenues) {
                for (const Timeframe tf : kTfs) {
                    // A timeframe can be switched off entirely; storage, not
                    // CPU, is the binding constraint.
                    if (!enabled_tfs_.empty() &&
                        !enabled_tfs_.contains(std::string(tf_label(tf)))) {
                        continue;
                    }
                    const ShardKey key{venue_id(venue), iid, tf_minutes(tf)};
                    if (shards_.contains(key)) continue;
                    auto sh = std::make_unique<Shard>();
                    sh->venue = venue;
                    sh->instrument_id = iid;
                    sh->symbol = sym;
                    sh->tf = tf;
                    sh->tf_label = std::string(tf_label(tf));
                    shards_[key] = std::move(sh);
                    startup.push_back(key);
                }
                }
            }
            // Every shard needs a catch-up pass after a restart, and the queue
            // drains in the order it is filled. Filled instrument by instrument,
            // the execution venue's 1m shard of the last instrument waited
            // behind ~750 others: AltML's predictions came back two instruments
            // a minute and the decision layer failed closed (ML_MISSING) for
            // eleven minutes after each restart (2026-09-25 09:24 and 10:26).
            // The execution venue goes first -- 1m, the grid every prediction
            // is made on, then the 15m and 1h grids the models align and the
            // regimes read -- and the information venues after it.
            const auto rank = [](const ShardKey& k) {
                const bool execution = std::get<0>(k) == venue_id(Venue::bitget);
                const int32_t tf = std::get<2>(k);
                const int tf_rank = tf == 1 ? 0 : (tf == 15 || tf == 60) ? 1 : 2;
                return (execution ? 0 : 3) + tf_rank;
            };
            std::stable_sort(startup.begin(), startup.end(),
                             [&](const ShardKey& a, const ShardKey& b) { return rank(a) < rank(b); });
            for (const auto& key : startup) {
                // We already hold mu_; enqueue_task() would try to lock it
                // again and deadlock before systemd readiness is reported.
                enqueue_task_locked(Task{{TaskMode::backfill, -1, 0}, key,
                                         std::chrono::steady_clock::now()});
            }
        }
        cv_.notify_all();
        log::info("feature_shards_initialized", {{"shards", shards_.size()}});
        return {};
    }

    // A divergence is a statement about the producer, so it is recorded where
    // an operator will find it — named, quantified, and never silent.
    void report_source_divergence(int64_t instrument_id, const std::string& tf_label,
                                  int64_t bar_open_time_ms, int64_t event_revision,
                                  int64_t persisted_revision, bool event_quality_ok,
                                  bool persisted_quality_ok, bool same_payload,
                                  uint64_t delivery_count) {
        divergences_.fetch_add(1, std::memory_order_relaxed);
        log::critical(
            "feature_source_divergence",
            {{"instrument_id", instrument_id},
             {"timeframe", tf_label},
             {"bar_open_time_ms", bar_open_time_ms},
             {"event_revision", event_revision},
             {"persisted_revision", persisted_revision},
             {"event_quality_ok", event_quality_ok},
             {"persisted_quality_ok", persisted_quality_ok},
             {"same_payload", same_payload},
             {"deliveries", static_cast<int64_t>(delivery_count)},
             {"action", "terminated; PostgreSQL treated as authoritative"},
             {"message",
              "a candle event disagreed with the authoritative store past the "
              "storage-wait bound; this is a producer-side inconsistency"}});
    }

    // Is a rebuild for this shard queued, or running right now?
    //
    // The reconciliation handler schedules a rebuild and then decides whether
    // to keep waiting. Without this it could not tell "the work I asked for is
    // still in progress" from "nothing is coming", and reported both as a
    // critical divergence. Measured on 2026-09-04: every one of 170 such
    // reports was followed by the vector landing 0.8-11 seconds later, from the
    // very rebuild the same handler had scheduled.
    [[nodiscard]] bool rebuild_outstanding(const ShardKey& key) {
        std::lock_guard lock(mu_);
        const auto sh = shards_.find(key);
        if (sh != shards_.end() && sh->second->busy) return true;
        for (const auto& t : tasks_) {
            if (t.key == key) return true;
        }
        return false;
    }

    void enqueue_task(Task t) {
        {
            std::lock_guard lock(mu_);
            enqueue_task_locked(std::move(t));
        }
        cv_.notify_all();
    }

    // Worker loop: executes backfill / catch-up / recompute tasks.
    void worker_loop() {
        while (!ShutdownToken::instance().stop_requested()) {
            Task task{};
            {
                std::unique_lock lock(mu_);
                cv_.wait_for(lock, std::chrono::seconds(1), [&] {
                    return !tasks_.empty() || ShutdownToken::instance().stop_requested();
                });
                if (ShutdownToken::instance().stop_requested()) return;
                if (tasks_.empty()) continue;
                // Find a ready task whose shard is free.
                const auto now = std::chrono::steady_clock::now();
                auto it = tasks_.begin();
                for (; it != tasks_.end(); ++it) {
                    auto& sh = shards_.at(it->key);
                    if (!sh->busy && it->ready_at <= now) break;
                }
                if (it == tasks_.end()) continue;
                task = *it;
                tasks_.erase(it);
                shards_.at(task.key)->busy = true;
            }
            TaskOutcome outcome;
            try {
                outcome = run_task(task);
            } catch (const std::exception& e) {
                outcome.error = std::string("unhandled feature task exception: ") + e.what();
            } catch (...) {
                outcome.error = "unhandled non-standard feature task exception";
            }
            unsigned retry_attempt = task.attempt;
            {
                std::lock_guard lock(mu_);
                shards_.at(task.key)->busy = false;
                if (!outcome.ok && !ShutdownToken::instance().stop_requested()) {
                    ++task.attempt;
                    retry_attempt = task.attempt;
                    task.ready_at = std::chrono::steady_clock::now() + retry_delay(task.attempt);
                    enqueue_task_locked(std::move(task));
                }
            }
            cv_.notify_all();
            if (!outcome.ok && !ShutdownToken::instance().stop_requested() &&
                (retry_attempt <= 3 || (retry_attempt & (retry_attempt - 1U)) == 0U)) {
                log::error("feature_task_failed",
                           {{"mode", task_mode_name(task.mode)},
                            {"message", outcome.error},
                            {"retry_attempt", retry_attempt}});
            }
        }
    }

    // Live closed candle from JetStream.  PostgreSQL is the ordering barrier:
    // the feature consumer may receive a stream message before the independent
    // storage-writer durable has committed it, so we NAK until that exact bar
    // is visible and only ACK after the vector is durable.
    Result<Delivery> on_live_candle(const ::alttech::v1::Envelope& env, const Candle& c,
                                    uint64_t delivery_count) {
        // Every venue is computed. The shard map is keyed on the canonical pair
        // id, which the venues share, so the venue is part of the key — see
        // ShardKey. An unrecognised venue is acknowledged rather than guessed.
        const auto venue_opt = venue_from_proto(env.exchange());
        if (!venue_opt) return Delivery::ack;
        const Venue venue = *venue_opt;
        // A bar older than the replay horizon will never get a vector: the
        // horizon is a storage decision and no partition exists that far back.
        // Retrying it would loop forever on data the service does not keep.
        const int64_t floor_ms =
            bounded_replay_floor_ms(c.timeframe, backfill_days_, timeutil::now_ms());
        if (floor_ms > 0 && c.open_time_ms < floor_ms) return Delivery::ack;

        const ShardKey key{venue_id(venue), env.instrument_id(), tf_minutes(c.timeframe)};
        const int64_t span = tf_ms(c.timeframe);
        std::string tf_name;
        {
            std::lock_guard lock(mu_);
            const auto it = shards_.find(key);
            if (it == shards_.end()) return Delivery::ack; // intentionally out of universe
            if (it->second->busy) return Delivery::retry;
            tf_name = it->second->tf_label;
        }

        auto persisted = load_persisted_candle(pool_, venue, env.instrument_id(), tf_name,
                                               c.open_time_ms, span);
        if (!persisted) return std::unexpected(persisted.error());
        if (!*persisted) return Delivery::retry;
        auto event_bar = to_fbar(c);
        if (!event_bar) return std::unexpected(event_bar.error());
        const auto barrier = source_barrier_decision(
            c.revision, (**persisted).revision,
            same_bar(*event_bar, (**persisted).bar) &&
                ((c.quality == Quality::ok) == (**persisted).quality_ok));
        if (barrier == SourceBarrierDecision::waiting_for_storage) {
            // The independent storage consumer has not committed this newer
            // event yet.  Retry until PostgreSQL catches up — but not forever
            // (service_logic.hpp explains what the bound is for).
            if (storage_barrier_exhausted(delivery_count, max_storage_wait_deliveries_)) {
                report_source_divergence(env.instrument_id(), tf_name, c.open_time_ms,
                                         c.revision, (**persisted).revision,
                                         (c.quality == Quality::ok), (**persisted).quality_ok,
                                         same_bar(*event_bar, (**persisted).bar),
                                         delivery_count);
                return Delivery::divergent;
            }
            retry_count_.fetch_add(1, std::memory_order_relaxed);
            return Delivery::retry;
        }
        if (barrier == SourceBarrierDecision::stale) {
            // PostgreSQL is authoritative.  An older retained JetStream
            // revision can never become current again; ACK it once the vector
            // for the persisted revision is durable, otherwise schedule the
            // current row for recomputation.
            auto exists = current_vector_exists(pool_, venue, env.instrument_id(), tf_name,
                                                 c.open_time_ms, feature_set_id_);
            if (!exists) return std::unexpected(exists.error());
            if (*exists) return Delivery::ack;
            enqueue_task(Task{{TaskMode::recompute, c.open_time_ms, 0}, key,
                              std::chrono::steady_clock::now()});
            return Delivery::retry;
        }
        if (!(**persisted).quality_ok) {
            // The in-memory engine appends bars one at a time and cannot mark
            // a single one INCOMPLETE_INPUT, so an under-filled bar is handed
            // to the DB replay instead.  That path keeps the bar in the series
            // as context and publishes INCOMPLETE_INPUT for the bar itself.
            auto exists = current_vector_exists(pool_, venue, env.instrument_id(), tf_name,
                                                 c.open_time_ms, feature_set_id_);
            if (!exists) return std::unexpected(exists.error());
            if (*exists) return Delivery::ack;
            enqueue_task(Task{{TaskMode::recompute, c.open_time_ms, 0}, key,
                              std::chrono::steady_clock::now()});
            return Delivery::retry;
        }

        std::unique_lock lock(mu_);
        const auto it = shards_.find(key);
        if (it == shards_.end()) return Delivery::ack;
        Shard& sh = *it->second;
        if (sh.busy) return Delivery::retry;

        if (sh.last_processed_ms >= 0 && c.open_time_ms <= sh.last_processed_ms) {
            const std::string symbol = sh.symbol;
            const Timeframe tf = sh.tf;
            const bool should_republish =
                c.open_time_ms == sh.last_processed_ms &&
                (sh.last_published_ms != c.open_time_ms ||
                 sh.last_published_revision != (**persisted).revision);
            lock.unlock();

            auto exists = current_vector_exists(pool_, venue, env.instrument_id(), tf_name,
                                                 c.open_time_ms, feature_set_id_);
            if (!exists) return std::unexpected(exists.error());
            if (!*exists) {
                enqueue_task(Task{{TaskMode::recompute, c.open_time_ms, 0}, key,
                                  std::chrono::steady_clock::now()});
                return Delivery::retry;
            }
            if (publish_live_ && should_republish) {
                auto durable = load_current_vector(
                    pool_, venue, env.instrument_id(), tf_name, c.open_time_ms, feature_set_id_,
                    (**persisted).revision);
                if (!durable) {
                    if (durable.error().code == ErrorCode::validation_error) {
                        log::error("durable_feature_vector_invalid",
                                   {{"symbol", symbol}, {"timeframe", tf_name},
                                    {"bar_open_time_ms", c.open_time_ms},
                                    {"message", durable.error().message}});
                        enqueue_task(Task{{TaskMode::recompute, c.open_time_ms, 0}, key,
                                          std::chrono::steady_clock::now()});
                        return Delivery::retry;
                    }
                    return std::unexpected(durable.error());
                }
                if (!*durable) {
                    enqueue_task(Task{{TaskMode::recompute, c.open_time_ms, 0}, key,
                                      std::chrono::steady_clock::now()});
                    return Delivery::retry;
                }
                if (auto published = publish_vector(venue, symbol, tf, env.instrument_id(), **durable);
                    !published) {
                    return std::unexpected(published.error());
                }
                std::lock_guard update(mu_);
                auto current = shards_.find(key);
                if (current != shards_.end() &&
                    current->second->last_processed_ms == c.open_time_ms) {
                    current->second->last_published_ms = c.open_time_ms;
                    current->second->last_published_revision = (**persisted).revision;
                }
            }
            return Delivery::ack;
        }
        if (sh.last_processed_ms >= 0 && c.open_time_ms != sh.last_processed_ms + span) {
            lock.unlock();
            enqueue_task(Task{{TaskMode::live, -2, 0}, key,
                              std::chrono::steady_clock::now()});
            return Delivery::retry;
        }

        // Keep the shared engine unchanged until its vector commits.  A failed
        // write can therefore be safely redelivered without skipping a bar.
        sh.busy = true;
        talib_features::FeatureEngine candidate = sh.engine;
        const std::string symbol = sh.symbol;
        const Timeframe tf = sh.tf;
        const bool was_ready = sh.live_ready;
        lock.unlock();

        auto vec = candidate.process_bar((**persisted).bar);
        VectorRow row;
        row.instrument_id = env.instrument_id();
        row.timeframe = tf_name;
        row.vec = std::move(vec);
        row.bar_volume = (**persisted).bar.base_volume;
        row.bar_revision = (**persisted).revision;
        row.quality = row.vec.status == talib_features::VectorStatus::valid ? "VALID" : "WARMING";
        row.computed_at_ms = timeutil::now_ms();
        const bool became_ready = !was_ready && row.vec.status == talib_features::VectorStatus::valid;

        auto write = write_vectors(pool_, venue, feature_set_id_, {row}, name_index_);
        if (!write) {
            std::lock_guard failed(mu_);
            auto current = shards_.find(key);
            if (current != shards_.end()) current->second->busy = false;
            cv_.notify_all();
            return std::unexpected(write.error());
        }
        if (auto checkpoint = save_checkpoint(pool_, venue, env.instrument_id(), tf_name, c.open_time_ms);
            !checkpoint) {
            log::warn("feature_checkpoint_write_failed",
                      {{"symbol", symbol}, {"timeframe", tf_name},
                       {"message", checkpoint.error().message}});
        }

        {
            std::lock_guard committed(mu_);
            auto current = shards_.find(key);
            if (current == shards_.end()) return Delivery::ack;
            current->second->engine = std::move(candidate);
            current->second->last_processed_ms = c.open_time_ms;
            current->second->last_row = row;
            current->second->live_ready = was_ready || became_ready;
            current->second->busy = false;
        }
        cv_.notify_all();

        if (publish_live_) {
            auto published = publish_vector(venue, symbol, tf, env.instrument_id(), row);
            if (!published) return std::unexpected(published.error());
            std::lock_guard update(mu_);
            auto current = shards_.find(key);
            if (current != shards_.end() && current->second->last_processed_ms == c.open_time_ms) {
                current->second->last_published_ms = c.open_time_ms;
                current->second->last_published_revision = row.bar_revision;
            }
        }
        if (became_ready) publish_status(symbol, tf, env.instrument_id(), "ready");
        live_count_.fetch_add(1, std::memory_order_relaxed);
        return Delivery::ack;
    }

    // Revised batch (recon/repair/backfill while live): rewind to earliest bar.
    Result<Delivery> on_batch(const ::alttech::v1::Envelope& env, Timeframe tf,
                              const std::vector<Candle>& candles, uint64_t delivery_count) {
        if (candles.empty()) {
            return fail(ErrorCode::validation_error, "empty candle batch");
        }
        const auto venue_opt = venue_from_proto(env.exchange());
        if (!venue_opt) return Delivery::ack;
        const Venue venue = *venue_opt;
        const int64_t floor_ms =
            bounded_replay_floor_ms(tf, backfill_days_, timeutil::now_ms());
        const ShardKey key{venue_id(venue), env.instrument_id(), tf_minutes(tf)};
        std::string tf_name;
        int64_t last_processed = -1;
        {
            std::lock_guard lock(mu_);
            const auto it = shards_.find(key);
            if (it == shards_.end()) return Delivery::ack;
            tf_name = it->second->tf_label;
            last_processed = it->second->last_processed_ms;
        }

        const int64_t span = tf_ms(tf);
        int64_t earliest_ms = candles.front().open_time_ms;
        int64_t latest_ms = earliest_ms;
        for (const auto& c : candles) latest_ms = std::max(latest_ms, c.open_time_ms);
        // What of this batch is actionable at all (service_logic.hpp explains
        // why a batch straddling the floor is the case that matters).
        const auto range = batch_processing_range(earliest_ms, latest_ms, floor_ms);
        if (!range.actionable) return Delivery::ack;
        if (range.clamped) {
            log::info("feature_batch_clamped_to_floor",
                      {{"instrument_id", env.instrument_id()},
                       {"timeframe", tf_name},
                       {"batch_earliest_ms", earliest_ms},
                       {"batch_latest_ms", latest_ms},
                       {"batch_size", static_cast<int64_t>(candles.size())},
                       {"deliveries", static_cast<int64_t>(delivery_count)},
                       {"replay_floor_ms", floor_ms}});
        }
        earliest_ms = range.from_ms;
        latest_ms = earliest_ms;
        struct ExpectedBar {
            talib_features::FBar bar;
            int64_t revision = 0;
            bool quality_ok = false;
        };
        std::map<int64_t, ExpectedBar> expected;
        for (const auto& c : candles) {
            if (!c.is_closed || c.timeframe != tf) {
                return fail(ErrorCode::validation_error,
                            "mixed timeframe or open candle in feature batch");
            }
            // A bar below the floor is not a bar this module will ever store, so
            // it must not enter the source-barrier check or the completeness
            // range either — both would then wait on a vector that cannot exist.
            if (floor_ms > 0 && c.open_time_ms < floor_ms) continue;
            auto b = to_fbar(c);
            if (!b) return std::unexpected(b.error());
            earliest_ms = std::min(earliest_ms, c.open_time_ms);
            latest_ms = std::max(latest_ms, c.open_time_ms);
            expected[c.open_time_ms] = ExpectedBar{*b, c.revision, c.quality == Quality::ok};
        }
        if (expected.empty()) {
            // Every bar in the batch fell below the floor. There is nothing to
            // verify and nothing to rebuild; retrying would be the same loop.
            return Delivery::ack;
        }
        if (latest_ms > std::numeric_limits<int64_t>::max() - span) {
            return fail(ErrorCode::validation_error, "batch candle range overflow");
        }
        // Verify exactly the bars this batch names. A ranged query with a limit
        // sized by the batch cannot do that for a sparse batch — see
        // candles_at_query().
        std::vector<int64_t> wanted_times;
        wanted_times.reserve(expected.size());
        for (const auto& [open_ms, _] : expected) wanted_times.push_back(open_ms);
        auto stored = load_candles_at(pool_, venue, env.instrument_id(), tf_name, wanted_times);
        if (!stored) return std::unexpected(stored.error());
        bool source_differs = false;
        for (const auto& [open_ms, wanted] : expected) {
            const auto got = std::find_if(stored->begin(), stored->end(), [&](const auto& row) {
                return row.bar.open_time_ms == open_ms;
            });
            if (got == stored->end()) {
                // The batch names a bar PostgreSQL has not stored. Bounded for
                // the same reason the revision barrier is: waiting forever on a
                // peer's state is how P0-1 happened.
                if (storage_barrier_exhausted(delivery_count, max_storage_wait_deliveries_)) {
                    report_source_divergence(env.instrument_id(), tf_name, open_ms,
                                             wanted.revision, -1, wanted.quality_ok, false,
                                             false, delivery_count);
                    enqueue_task(Task{{TaskMode::recompute, earliest_ms, 0}, key,
                                      std::chrono::steady_clock::now()});
                    return Delivery::divergent;
                }
                retry_count_.fetch_add(1, std::memory_order_relaxed);
                return Delivery::retry;
            }
            const auto barrier = source_barrier_decision(
                wanted.revision, got->revision,
                same_bar(wanted.bar, got->bar) &&
                    (wanted.quality_ok == got->quality_ok));
            if (barrier == SourceBarrierDecision::waiting_for_storage) {
                if (storage_barrier_exhausted(delivery_count, max_storage_wait_deliveries_)) {
                    report_source_divergence(env.instrument_id(), tf_name, open_ms,
                                             wanted.revision, got->revision,
                                             wanted.quality_ok, got->quality_ok,
                                             same_bar(wanted.bar, got->bar), delivery_count);
                    // The authoritative store is PostgreSQL. Make the vectors
                    // agree with it and stop waiting on an event that never will.
                    enqueue_task(Task{{TaskMode::recompute, earliest_ms, 0}, key,
                                      std::chrono::steady_clock::now()});
                    return Delivery::divergent;
                }
                retry_count_.fetch_add(1, std::memory_order_relaxed);
                return Delivery::retry;
            }
            if (barrier == SourceBarrierDecision::stale) source_differs = true;
        }

        auto complete = current_vectors_complete(pool_, venue, env.instrument_id(), tf_name,
                                                 earliest_ms, latest_ms + span,
                                                 feature_set_id_);
        if (!complete) return std::unexpected(complete.error());
        if (*complete) return Delivery::ack;

        // WHAT to rebuild, not merely WHETHER to. The batch names a rolling
        // reconciliation horizon that AltSpotter re-announces so a late 1m
        // revision cannot be missed, so it routinely overlaps bars that are
        // already featured and current. Rebuilding from its first bar because
        // it overlaps history turned one genuinely new bar into a full
        // horizon rebuild — seven vectors per symbol every five minutes on the
        // 5m timeframe, permanently, on a box that has better uses for the CPU.
        //
        // `source_differs` is the precise signal that the batch CONTRADICTS
        // what the features were built on; only that justifies rebuilding the
        // whole overlap. Otherwise the work is exactly the bars that lack a
        // current vector, and the earliest of those is where the rebuild starts.
        int64_t rebuild_from = earliest_ms;
        if (!source_differs) {
            auto missing = first_incomplete_bar(pool_, venue, env.instrument_id(), tf_name, earliest_ms,
                                                latest_ms + span, feature_set_id_);
            if (!missing) return std::unexpected(missing.error());
            if (!*missing) {
                // Nothing is missing after all: the completeness probe and this
                // one disagree only if a vector landed between the two queries,
                // which is a race the ACK resolves correctly.
                return Delivery::ack;
            }
            rebuild_from = **missing > earliest_ms ? **missing : earliest_ms;
        }

        if (source_differs || rebuild_from <= last_processed) {
            enqueue_task(Task{{TaskMode::recompute, rebuild_from, 0}, key,
                              std::chrono::steady_clock::now()});
        } else {
            // Everything the batch names below `rebuild_from` is already
            // current, and the engine's own position is behind it: a forward
            // append is both sufficient and an order of magnitude cheaper.
            enqueue_task(Task{{TaskMode::live, -2, 0}, key,
                              std::chrono::steady_clock::now()});
        }
        // The rebuild was scheduled; waiting for it is legitimate. Waiting for
        // it indefinitely is not — if the range still will not complete after
        // the bound, the rebuild is not going to fix it and the message must
        // stop occupying the ack-pending budget.
        if (storage_barrier_exhausted(delivery_count, max_storage_wait_deliveries_)) {
            // Two different states used to share one CRITICAL line, which is
            // what made a real stall indistinguishable from ordinary lateness.
            //
            // LATE: the rebuild this handler scheduled is queued or running. The
            // range is not unresolvable, it is unfinished -- measured, the
            // vector lands within seconds. Nothing is lost by acking: the
            // producer re-announces a rolling reconciliation horizon, so the
            // next announcement re-checks the same range, and if the rebuild
            // really did fail it will arrive here again with nothing in flight
            // and take the branch below.
            if (rebuild_outstanding(key)) {
                log::info("feature_batch_rebuild_pending",
                          {{"instrument_id", env.instrument_id()},
                           {"timeframe", tf_name},
                           {"range_from_ms", earliest_ms},
                           {"range_to_ms", latest_ms + span},
                           {"deliveries", static_cast<int64_t>(delivery_count)},
                           {"action", "acked; rebuild still in flight, next "
                                      "announcement re-checks the range"}});
                return Delivery::ack;
            }
            // DIVERGENT: the budget is spent and nothing is working on it. This
            // is the state worth waking someone for.
            log::critical("feature_batch_never_completes",
                          {{"instrument_id", env.instrument_id()},
                           {"timeframe", tf_name},
                           {"range_from_ms", earliest_ms},
                           {"range_to_ms", latest_ms + span},
                           {"deliveries", static_cast<int64_t>(delivery_count)},
                           {"action", "terminated; no rebuild in flight"},
                           {"message",
                            "the completeness range did not resolve and no "
                            "rebuild is queued or running for this shard"}});
            divergences_.fetch_add(1, std::memory_order_relaxed);
            return Delivery::divergent;
        }
        retry_count_.fetch_add(1, std::memory_order_relaxed);
        return Delivery::retry;
    }

    void log_stats() {
        size_t task_count = 0;
        {
            std::lock_guard lock(mu_);
            task_count = tasks_.size();
        }
        const int64_t live = live_count_.exchange(0);
        const int64_t retried = retry_count_.exchange(0);
        log::info("feature_stats", {{"live_vectors", live},
                                    {"retried_deliveries", retried},
                                    {"divergences", divergences_.load()},
                                    {"task_queue", task_count}});

        // Publishing nothing while asking for redeliveries is the signature of a
        // batch the module can never satisfy. It is not a slow patch and it does
        // not clear on its own, so it is reported as a fault rather than left to
        // be inferred from a flat counter.
        if (live == 0 && retried > 0) {
            const int64_t quiet = quiet_intervals_.fetch_add(1) + 1;
            if (quiet >= 3) {
                log::critical("feature_pipeline_stalled",
                              {{"quiet_intervals", quiet},
                               {"retried_deliveries", retried},
                               {"task_queue", task_count},
                               {"detail", "no live feature vector published while deliveries "
                                          "are being retried; downstream regime, structure and "
                                          "AltStrategy context are starved"}});
            }
        } else if (live > 0) {
            quiet_intervals_.store(0);
        }
    }

    // The reference venues have no live driver. AltSpotter publishes bar events
    // only for the primary venue, so on_live_candle() can never fire for the
    // other three: they were backfilled once at startup and then stood still,
    // which left three quarters of the venue coverage frozen while still
    // looking present. Their candles do arrive -- AltSpotter persists them and
    // the FDW exposes them -- so what is missing is the trigger, not the data.
    // This supplies the trigger the event path supplies for the primary venue,
    // at the cadence the venue's own bars close and no faster: a bar opened at
    // T is only processable once T + 2 spans have passed, because that is when
    // the bar after it has both opened and closed.
    void enqueue_reference_catchup() {
        const int64_t now = timeutil::now_ms();
        int64_t queued = 0;
        {
            std::lock_guard lock(mu_);
            for (auto& [key, sh] : shards_) {
                if (!is_reference(sh->venue) || sh->busy) continue;
                if (sh->last_processed_ms < 0) continue;   // startup backfill owns it
                if (now < sh->last_processed_ms + 2 * tf_ms(sh->tf)) continue;
                // enqueue_task_locked() merges into a pending task for the same
                // shard, so a slow shard cannot accumulate a queue of its own.
                enqueue_task_locked(Task{{TaskMode::live, -2, 0}, key,
                                         std::chrono::steady_clock::now()});
                ++queued;
            }
        }
        if (queued > 0) {
            cv_.notify_all();
            log::info("feature_reference_catchup", {{"shards", queued}});
        }
    }

private:
    static Result<talib_features::FBar> to_fbar(const Candle& c) {
        try {
            talib_features::FBar b;
            b.open_time_ms = c.open_time_ms;
            // Feature domain works in double; exact decimals remain in candles.
            b.open = std::stod(c.open.to_string());
            b.high = std::stod(c.high.to_string());
            b.low = std::stod(c.low.to_string());
            b.close = std::stod(c.close.to_string());
            b.base_volume = std::stod(c.base_volume.to_string());
            b.quote_volume = std::stod(c.quote_volume.to_string());
            const bool finite = std::isfinite(b.open) && std::isfinite(b.high) &&
                                std::isfinite(b.low) && std::isfinite(b.close) &&
                                std::isfinite(b.base_volume) && std::isfinite(b.quote_volume);
            if (!finite || b.open_time_ms < 0 || b.open <= 0 || b.high <= 0 || b.low <= 0 ||
                b.close <= 0 || b.base_volume < 0 || b.quote_volume < 0 ||
                b.high < std::max(b.open, b.close) || b.low > std::min(b.open, b.close) ||
                b.low > b.high) {
                return fail(ErrorCode::validation_error, "invalid candle values");
            }
            return b;
        } catch (const std::exception& e) {
            return fail(ErrorCode::validation_error,
                        std::string("invalid candle decimal: ") + e.what());
        }
    }

    static bool same_bar(const talib_features::FBar& a, const talib_features::FBar& b) {
        const auto close = [](double x, double y) {
            const double scale = std::max({1.0, std::abs(x), std::abs(y)});
            return std::abs(x - y) <= scale * 1e-12;
        };
        return a.open_time_ms == b.open_time_ms && close(a.open, b.open) &&
               close(a.high, b.high) && close(a.low, b.low) && close(a.close, b.close) &&
               close(a.base_volume, b.base_volume) && close(a.quote_volume, b.quote_volume);
    }

    // `venue` is a parameter, not an assumption.
    //
    // The subject was built with a literal Venue::bitget and the envelope was
    // stamped EXCHANGE_BITGET, while this module computes all four venues. Every
    // reference-venue vector therefore went out as Bitget's -- wrong subject and
    // wrong payload -- so a downstream consumer could not tell them apart and
    // AltML's live path saw one venue no matter how many AltTech computed.
    Result<void> publish_vector(Venue venue, const std::string& symbol, Timeframe tf,
                                int64_t instrument_id, const VectorRow& row) {
        messaging::EnvelopeMeta meta;
        meta.event_type = "feature.core";
        meta.venue = venue;
        meta.instrument_id = instrument_id;
        meta.exchange_symbol = symbol;
        meta.exchange_timestamp_ms = row.vec.bar_open_time_ms;
        meta.source_service = "alttech-service";
        // The venue belongs in the id: without it the four venues' vectors for
        // the same bar share a message id, and JetStream's dedup window drops
        // three of them as duplicates of the first.
        meta.message_id = "fv:" + std::string(venue_code(venue)) + ":" + symbol + ":" +
                          row.timeframe + ":" + std::to_string(row.vec.bar_open_time_ms) + ":r" +
                          std::to_string(row.bar_revision);
        auto env = messaging::make_envelope(meta);
        auto* fv = env.mutable_feature_vector();
        fv->set_timeframe(static_cast<::alttech::v1::Timeframe>(tf_minutes(tf)));
        fv->set_bar_open_time_ms(row.vec.bar_open_time_ms);
        fv->set_bar_revision(row.bar_revision);
        fv->set_feature_set_name(talib_features::kCoreSetName);
        fv->set_feature_set_version(talib_features::kCoreSetVersion);
        fv->set_schema_hash(talib_features::core_schema_fingerprint());
        fv->set_engine_version(row.engine_version);
        for (const float v : row.vec.values) fv->add_values(v);
        fv->set_validity_bitmap(std::string(row.vec.validity.begin(), row.vec.validity.end()));
        switch (row.vec.status) {
            case talib_features::VectorStatus::not_ready:
                fv->set_status(::alttech::v1::FEATURE_STATUS_NOT_READY);
                break;
            case talib_features::VectorStatus::warming:
                fv->set_status(::alttech::v1::FEATURE_STATUS_WARMING);
                break;
            case talib_features::VectorStatus::valid:
                fv->set_status(::alttech::v1::FEATURE_STATUS_VALID);
                break;
            case talib_features::VectorStatus::incomplete_input:
                fv->set_status(::alttech::v1::FEATURE_STATUS_INCOMPLETE_INPUT);
                break;
            case talib_features::VectorStatus::revised:
                fv->set_status(::alttech::v1::FEATURE_STATUS_REVISED);
                break;
            case talib_features::VectorStatus::invalid:
                fv->set_status(::alttech::v1::FEATURE_STATUS_INVALID);
                break;
        }
        fv->set_computed_at_ms(row.computed_at_ms);
        return nats_.js_publish(
            messaging::topology::venue_feature_subject(venue, symbol, tf),
            messaging::serialize(env), env.message_id());
    }

    void publish_status(const std::string& symbol, Timeframe tf, int64_t instrument_id,
                        const std::string& kind) {
        messaging::EnvelopeMeta meta;
        meta.event_type = "status.feature." + kind;
        meta.instrument_id = instrument_id;
        meta.exchange_symbol = symbol;
        const int64_t observed_at_ms = timeutil::now_ms();
        meta.exchange_timestamp_ms = observed_at_ms;
        meta.source_service = "alttech-service";
        meta.message_id = "fstat:" + symbol + ":" + std::string(tf_label(tf)) + ":" + kind + ":" +
                          std::to_string(observed_at_ms / 60000);
        auto env = messaging::make_envelope(meta);
        auto* status = env.mutable_status_event();
        status->set_module(::alttech::v1::ANALYTICS_MODULE_FEATURE);
        status->set_status(kind == "ready" ? ::alttech::v1::MODULE_STATUS_READY
                                            : ::alttech::v1::MODULE_STATUS_RECOMPUTED);
        status->set_timeframe(static_cast<::alttech::v1::Timeframe>(tf_minutes(tf)));
        status->set_observed_at_ms(observed_at_ms);
        status->set_detail(kind);
        const std::string subject = messaging::topology::status_subject(
            "feature", Venue::bitget, symbol, tf_label(tf), kind);
        if (auto published = nats_.js_publish(subject, messaging::serialize(env), env.message_id());
            !published) {
            log::warn("feature_status_publish_failed",
                      {{"symbol", symbol}, {"timeframe", std::string(tf_label(tf))},
                       {"kind", kind}, {"message", published.error().message}});
        }
    }

    // How many closed source bars must precede a WARMING vector before its
    // status is treated as stale rather than genuine. Measured on a real
    // series, the fast indicator ladder passes kMinUsableFraction between 40
    // and 50 bars, so 60 is past the threshold without being tied to its exact
    // value -- and a vector that still cannot converge with 60 bars behind it
    // is WARMING for a real reason.
    static constexpr int64_t kStaleWarmingContextBars = 60;

    TaskOutcome run_task(const Task& task) {
        Shard* sh = nullptr;
        {
            std::lock_guard lock(mu_);
            sh = shards_.at(task.key).get();
        }
        const int64_t span = tf_ms(sh->tf);
        const int64_t replay_floor =
            bounded_replay_floor_ms(sh->tf, backfill_days_, timeutil::now_ms());
        int64_t resume_from = 0; // first bar whose vector will be (re)written

        if (task.mode == TaskMode::recompute && task.from_ms >= 0) {
            resume_from = task.from_ms;
        } else if (task.from_ms == -2 && sh->last_processed_ms >= 0) {
            resume_from = sh->last_processed_ms + span; // pure catch-up
        } else {
            // BACKFILL starts at the earliest missing current-revision vector.
            // This repairs historical holes instead of looking only at max().
            auto missing = first_missing_vector_time(pool_, sh->venue, sh->instrument_id, sh->tf_label,
                                                     feature_set_id_, replay_floor);
            if (!missing) return {false, 0, missing.error().message};
            if (*missing) {
                resume_from = **missing;
            } else {
                // No holes -- but a PRESENT vector can still be out of date.
                // Vectors record the history that existed when they were
                // computed, and that history grows underneath them: when
                // AltSpotter went from 60 to 280 days of 1m, every 1d vector
                // already written stayed WARMING with 733 of 995 features,
                // because nothing looks at vectors that exist. AltML copies
                // only VALID, so those bars remained invisible downstream with
                // the data to fix them sitting in the source table.
                //
                // Recomputing is safe and self-limiting: a vector that is
                // WARMING for a real reason (it follows a gap) recomputes to
                // the same status and the context test below stops it being
                // picked again.
                auto stale = first_stale_warming_vector_time(
                    pool_, sh->venue, sh->instrument_id, sh->tf_label, feature_set_id_,
                    replay_floor, kStaleWarmingContextBars);
                if (!stale) return {false, 0, stale.error().message};
                if (*stale && **stale != sh->last_stale_from_ms) {
                    log::info("feature_stale_warming_recompute",
                              {{"symbol", sh->symbol},
                               {"timeframe", sh->tf_label},
                               {"from_ms", **stale}});
                    {
                        std::lock_guard lock(mu_);
                        sh->last_stale_from_ms = **stale;
                    }
                    resume_from = **stale;
                    // More may remain above this bar; run the sweep again once
                    // this pass lands. The guard above stops it repeating on a
                    // bar that cannot improve.
                    enqueue_task(Task{{TaskMode::backfill, -1, 0}, task.key,
                                      std::chrono::steady_clock::now() + std::chrono::seconds(5)});
                } else {
                    // Genuinely complete: replay warmup and continue after the
                    // latest vector. This restores the in-memory engine after
                    // every restart.
                    auto last = last_vector_time(pool_, sh->venue, sh->instrument_id,
                                                 sh->tf_label, feature_set_id_);
                    if (!last) return {false, 0, last.error().message};
                    resume_from = *last ? **last + span : 0;
                }
            }
        }

        const char* mode = task_mode_name(task.mode);
        auto run_id = open_run(pool_, sh->venue, mode, sh->instrument_id, sh->tf_label);
        if (!run_id) return {false, 0, run_id.error().message};
        int64_t written = 0;
        std::string error;

        // The replay floor bounds what may be WRITTEN: a vector can only be
        // stored where a partition exists, and partitions only cover the
        // retention window. A shard with nothing to resume from therefore
        // starts writing at the floor itself.
        if (resume_from <= 0) resume_from = replay_floor;

        // ...and the floor binds EVERY path, not just the empty one. A RECOMPUTE
        // task carries its own from_ms, and that value used to be trusted
        // verbatim. A gap-repair event raised during an outage names bars from
        // the outage window; by the time the task runs, retention has dropped
        // those partitions, so the insert fails with 23514 "no partition of
        // relation ... found for row" — an error that can never succeed, yet the
        // task is retried forever. Observed in production: retry_attempt 128 and
        // climbing on a bar from five days earlier, each attempt replaying the
        // whole history and emitting thousands of feature_replay_gap_reset
        // warnings, pinning one core at ~70 % and starving the LIVE queue so that
        // no fresh features were published at all.
        //
        // Clamping forward is the correct repair, not skipping: the request to
        // rebuild this shard is still honoured, just from the oldest bar that can
        // actually be stored.
        if (resume_from < replay_floor) {
            log::info("feature_replay_clamped_to_floor",
                      {{"symbol", sh->symbol},
                       {"timeframe", sh->tf_label},
                       {"requested_from_ms", resume_from},
                       {"replay_floor_ms", replay_floor}});
            resume_from = replay_floor;
        }

        // Warm the engine from scratch: replay warmup window before resume_from.
        // The warm-up span is a READ of context bars that are never written
        // (only bars >= resume_from are emitted below), so it must NOT be
        // clamped by the write floor. Clamping it truncated the context to
        // `backfill_days` worth of bars, which is fewer than
        // FeatureEngine::kMinValidBars for every timeframe >= 15m — two days is
        // 192 bars at 15m against a 250-bar threshold — so those timeframes
        // could never leave WARMING, and every consumer that gates on a VALID
        // vector (regime, structure, the cross-venue projection) saw nothing.
        talib_features::FeatureEngine engine;
        const int64_t warm_from =
            resume_from - static_cast<int64_t>(talib_features::FeatureEngine::kWarmupBars) * span;
        int64_t cursor = std::max<int64_t>(warm_from, 0);
        int64_t last_ms = -1;
        std::vector<VectorRow> pending;
        std::optional<VectorRow> latest_row;

        // TA-Lib is a batch library: recomputing a 2048-bar window per bar costs
        // ~86 ms, which makes a per-bar backfill take days. Bars are therefore
        // computed in ONE pass per contiguous run (~1800x faster). Gaps and bad
        // input still split the batch and reset the warm-up context, exactly as
        // the per-bar replay did.
        std::deque<talib_features::FBar> history;   // warm-up context across chunks
        std::vector<ShardCandle> run;               // current contiguous run

        const auto flush_run = [&]() -> bool {
            if (run.empty()) return true;
            std::vector<talib_features::FBar> series(history.begin(), history.end());
            const size_t ctx = series.size();
            for (const auto& c : run) series.push_back(c.bar);

            const auto vecs = talib_features::FeatureEngine::process_series(series);
            if (vecs.size() != series.size()) {
                error = "feature batch size mismatch";
                return false;
            }
            for (size_t i = 0; i < run.size(); ++i) {
                const auto& c = run[i];
                VectorRow row;
                row.instrument_id = sh->instrument_id;
                row.timeframe = sh->tf_label;
                if (c.quality_ok) {
                    row.vec = vecs[ctx + i];
                } else {
                    // The bar carried the series (its OHLC is the real open,
                    // close and extremes of the window) but its own volume is
                    // understated, so it publishes no feature values of its own.
                    row.vec.bar_open_time_ms = c.bar.open_time_ms;
                    row.vec.values.assign(talib_features::feature_count(), 0.0F);
                    row.vec.validity.assign((talib_features::feature_count() + 7U) / 8U, 0);
                    row.vec.status = talib_features::VectorStatus::incomplete_input;
                }
                row.bar_volume = c.bar.base_volume;
                row.bar_revision = c.revision;
                row.quality = published_vector_quality(
                    c.quality_ok, row.vec.status == talib_features::VectorStatus::valid);
                row.computed_at_ms = timeutil::now_ms();
                latest_row = row;
                if (c.bar.open_time_ms >= resume_from) {
                    pending.push_back(std::move(row));
                    if (pending.size() >= write_batch_) {
                        auto w = write_vectors(pool_, sh->venue, feature_set_id_, pending, name_index_);
                        if (!w) {
                            error = w.error().message;
                            return false;
                        }
                        written += *w;
                        pending.clear();
                    }
                }
            }
            for (const auto& c : run) {
                history.push_back(c.bar);
                if (history.size() > talib_features::FeatureEngine::kWarmupBars) {
                    history.pop_front();
                }
            }
            run.clear();
            return true;
        };

        while (!ShutdownToken::instance().stop_requested()) {
            auto chunk = load_candles(pool_, sh->venue, sh->instrument_id, sh->tf_label, cursor,
                                      std::numeric_limits<int64_t>::max(), chunk_);
            if (!chunk) {
                error = chunk.error().message;
                break;
            }
            if (chunk->empty()) break;
            for (const auto& c : *chunk) {
                const bool gap = last_ms >= 0 && c.bar.open_time_ms != last_ms + span;
                last_ms = c.bar.open_time_ms;

                // A GAP breaks continuity: a bar is absent, the series is no
                // longer contiguous, and every windowed indicator downstream
                // would silently span the hole. The context must be discarded.
                //
                // An INCOMPLETE bar is a different thing entirely, and treating
                // the two alike is what kept every timeframe >= 15m permanently
                // in WARMING. The bar EXISTS: AltSpotter aggregates it from the
                // 1m candles it has and marks it `incomplete` when fewer than
                // `expected` minutes contributed. Its OHLC is still the real
                // open, close and extremes of the window; only volume is
                // understated. Measured on this deployment, such bars carry 10
                // to 14 of 15 minutes — never a fragment.
                //
                // Discarding 250 bars of context because one of them was built
                // from 14 minutes instead of 15 made a VALID vector
                // unreachable: 23% of 15m bars are incomplete here (741 of 960
                // over ten days), so the longest unbroken run of `ok` bars is
                // 77 against a 250-bar threshold. AltStrategy decides on 15m,
                // so it could never assemble a DecisionContext at all.
                //
                // The bar therefore stays in the warm-up context, and the
                // vector emitted FOR it is still marked INCOMPLETE_INPUT below —
                // the quality of that bar is reported, not hidden, and the
                // continuity of the window is preserved.
                if (!warmup_context_survives(gap, c.quality_ok)) {
                    if (!flush_run()) break;
                    history.clear();   // warm-up context is no longer continuous
                    log::warn("feature_replay_gap_reset",
                              {{"symbol", sh->symbol},
                               {"timeframe", sh->tf_label},
                               {"bar_open_time_ms", c.bar.open_time_ms}});
                }
                // Under-filled bars stay in the run. Dropping them left the
                // indicator series time-discontiguous — every window silently
                // skipped a genuine price observation — while flush_run still
                // publishes INCOMPLETE_INPUT for the bar itself.
                run.push_back(c);
            }
            if (error.empty() && !flush_run()) { /* error set inside */ }
            if (!error.empty()) break;
            cursor = chunk->back().bar.open_time_ms + 1;
            if (chunk->size() < static_cast<size_t>(chunk_)) break; // caught up
        }
        if (error.empty() && !pending.empty()) {
            auto w = write_vectors(pool_, sh->venue, feature_set_id_, pending, name_index_);
            if (!w) error = w.error().message;
            else written += *w;
        }

        // Publish the freshly-computed tail.
        //
        // This path WROTE vectors and never published them. That was invisible
        // while it only ever ran for the primary venue's history, but the
        // reference venues are driven entirely from here -- AltSpotter's bar
        // events reach only bar.bitget.> -- so their vectors reached PostgreSQL
        // and never NATS. A consumer on the live subjects saw one venue however
        // many AltTech computed.
        //
        // Only genuinely live bars go out: a bar is publishable while it is
        // younger than two of its own spans. Replaying months of history onto
        // the stream would be a different bug wearing this fix's clothes.
        if (error.empty() && publish_live_ && !pending.empty()) {
            const int64_t span = tf_ms(sh->tf);
            const int64_t now_ms = timeutil::now_ms();
            const auto& last = pending.back();
            if (now_ms - last.vec.bar_open_time_ms <= 2 * span) {
                if (auto published = publish_vector(sh->venue, sh->symbol, sh->tf,
                                                    sh->instrument_id, last);
                    !published) {
                    log::warn("feature_worker_publish_failed",
                              {{"venue", std::string(venue_code(sh->venue))},
                               {"symbol", sh->symbol},
                               {"timeframe", sh->tf_label},
                               {"message", published.error().message}});
                }
            }
        }

        // process_series() is intentionally stateless. Rebuild the live facade
        // from the final contiguous trailing run before publishing the shard as
        // caught up; otherwise the next live candle would restart from an empty
        // window and incorrectly return to WARMING after every DB replay.
        if (error.empty()) {
            const std::vector<talib_features::FBar> trailing(history.begin(), history.end());
            if (auto restored = engine.restore_state(trailing, span); !restored) {
                error = restored.error().message;
            }
        }

        if (error.empty() && last_ms >= 0) {
            auto checkpoint = save_checkpoint(pool_, sh->venue, sh->instrument_id, sh->tf_label, last_ms);
            if (!checkpoint) error = checkpoint.error().message;
        }
        close_run(pool_, *run_id, written, error.empty(), error);
        if (!error.empty()) {
            return {false, written, error};
        }

        {
            std::lock_guard lock(mu_);
            sh->engine = std::move(engine);
            if (last_ms >= 0) sh->last_processed_ms = last_ms;
            sh->last_row = std::move(latest_row);
            sh->live_ready = sh->last_row &&
                             sh->last_row->vec.status == talib_features::VectorStatus::valid;
        }
        if (written > 0) {
            log::info("feature_task_done", {{"symbol", sh->symbol},
                                            {"timeframe", sh->tf_label},
                                            {"mode", mode},
                                            {"vectors", written}});
            if (task.mode == TaskMode::recompute) {
                publish_status(sh->symbol, sh->tf, sh->instrument_id, "recomputed");
            }
        }
        return {true, written, {}};
    }

    persistence::PgPool& pool_;
    messaging::NatsClient& nats_;
    int64_t feature_set_id_;
    bool publish_live_ = true;
    int chunk_ = 5000;
    size_t write_batch_ = 400;
    int64_t retry_min_ms_ = 1000;
    int64_t retry_max_ms_ = 30000;
    std::map<std::string, size_t> name_index_;

    std::mutex mu_;
    std::condition_variable cv_;
    std::map<ShardKey, std::unique_ptr<Shard>> shards_;
    std::deque<Task> tasks_;
    std::atomic<int64_t> live_count_{0};
    // Deliveries this module asked JetStream to send again. A batch that can
    // never be satisfied shows up here and nowhere else: the task queue drains
    // normally, the tasks complete normally, and only the redelivery count
    // keeps climbing. Without it, the module reported itself healthy for 6.8
    // days while publishing nothing.
    std::atomic<int64_t> retry_count_{0};
    std::atomic<int64_t> quiet_intervals_{0};
    std::atomic<int64_t> divergences_{0};
    // How many JetStream deliveries a cross-consumer storage barrier may spend
    // waiting before the disagreement is treated as divergence. 0 disables the
    // bound, restoring the previous unbounded behaviour.
    int64_t max_storage_wait_deliveries_ = 20;

    void enqueue_task_locked(Task task) {
        for (auto& pending : tasks_) {
            if (pending.key != task.key) continue;
            merge_pending_task(pending, task);
            pending.ready_at = std::min(pending.ready_at, task.ready_at);
            return;
        }
        tasks_.push_back(std::move(task));
    }

    std::chrono::milliseconds retry_delay(unsigned attempt) const {
        const unsigned shift = std::min(attempt > 0 ? attempt - 1U : 0U, 10U);
        const int64_t factor = int64_t{1} << shift;
        const int64_t base = std::min(retry_max_ms_, retry_min_ms_ * factor);
        const int64_t jitter_span = std::max<int64_t>(1, base / 5);
        const int64_t jitter = static_cast<int64_t>((attempt * 2654435761U) %
                                                    static_cast<unsigned>(jitter_span));
        return std::chrono::milliseconds(std::min(retry_max_ms_, base + jitter));
    }
};

} // namespace

namespace alttech::modules {

Result<void> run_feature(const YAML::Node& service_config,
                         persistence::PgPool& pool,
                         messaging::NatsClient& nats,
                         application::ModuleSupervisor& supervisor) {
    const auto fail_start = [&](const Error& error) -> Result<void> {
        supervisor.mark_failed(kModuleName, error);
        return std::unexpected(error);
    };

    const int64_t workers_config = config::opt_int(service_config, "workers", 2);
    const int64_t backfill_chunk = config::opt_int(service_config, "backfill_chunk", 5000);
    const int64_t write_batch = config::opt_int(service_config, "write_batch", 400);
    const int64_t backfill_days = config::opt_int(service_config, "backfill_days", 2);
    const int64_t retry_min =
        config::opt_int(service_config, "task_retry_backoff_min_ms", 1000);
    const int64_t retry_max =
        config::opt_int(service_config, "task_retry_backoff_max_ms", 30000);
    const std::string bar_stream = config::opt_str(
        service_config, "bar_stream",
        std::string(messaging::topology::kDefaultInputStream));
    const std::string bar_consumer = config::opt_str(
        service_config, "bar_consumer",
        std::string(messaging::topology::kDefaultBarConsumer));
    if (workers_config < 1 || workers_config > 16 ||
        backfill_chunk < 1 || backfill_chunk > 100'000 ||
        write_batch < 1 || write_batch > 2000 ||
        backfill_days < 1 || backfill_days > 365 ||
        retry_min < 100 || retry_min > 60'000 ||
        retry_max < retry_min || retry_max > 300'000 ||
        !messaging::topology::valid_resource_name(bar_stream) ||
        !messaging::topology::valid_resource_name(bar_consumer)) {
        const Error error{ErrorCode::config_error,
                          "invalid feature module limits or JetStream resource names"};
        log::error("invalid_feature_module_config",
                   {{"workers", workers_config},
                    {"backfill_chunk", backfill_chunk},
                    {"write_batch", write_batch},
                    {"backfill_days", backfill_days},
                    {"task_retry_backoff_min_ms", retry_min},
                    {"task_retry_backoff_max_ms", retry_max},
                    {"bar_stream", bar_stream},
                    {"bar_consumer", bar_consumer}});
        return fail_start(error);
    }
    const int workers = static_cast<int>(workers_config);

    // Fail closed on a missing schema: an analytics module must never run
    // against a database it cannot write its outputs to.
    if (auto probe = pool.with_txn([](pqxx::work& txn) {
            const auto row = txn.exec1(
                "SELECT to_regclass('exchanges') IS NOT NULL "
                "AND to_regclass('exchange_instruments') IS NOT NULL "
                "AND to_regclass('market_candles_1m') IS NOT NULL "
                "AND to_regclass('market_candles_agg') IS NOT NULL "
                "AND to_regclass('alttech.feature_set_versions') IS NOT NULL "
                "AND to_regclass('alttech.venue_feature_vectors') IS NOT NULL "
                "AND to_regclass('alttech.venue_feature_projection') IS NOT NULL "
                "AND to_regclass('alttech.module_checkpoints') IS NOT NULL");
            if (!row[0].as<bool>()) {
                throw std::runtime_error(
                    "required market-data or alttech feature schema is missing");
            }
        });
        !probe) {
        log::critical("postgres_schema_unavailable", {{"message", probe.error().message}});
        return fail_start(probe.error());
    }

    // Make the partition coverage match this module's own replay horizon. A
    // write outside an existing partition fails, so the horizon and the
    // partitions must never be configured independently of each other.
    // Same AccessExclusiveLock as the hourly maintenance run, so it yields the
    // same way; at start-up there is nothing to yield to yet, but this path also
    // runs while the service is live.
    // Retried, not fatal. `CREATE TABLE ... PARTITION OF` needs an
    // AccessExclusiveLock on the parent while the feature writers hold
    // AccessShareLock on it, so under load this times out — and a lock timeout
    // is transient by construction: the writers finish. Treating it as fatal
    // killed the service three times in a row once the engine began computing
    // four venues instead of one, simply because there were four times as many
    // writers to contend with. Give it several attempts with a widening gap
    // before declaring the schema unusable.
    bool partition_ensure_deferred_at_start = false;
    {
        constexpr int kAttempts = 6;
        Error last{ErrorCode::internal_error, "partition ensure not attempted"};
        bool ok = false;
        for (int attempt = 1; attempt <= kAttempts && !ok; ++attempt) {
            auto partitions = pool.with_txn([&](pqxx::work& txn) {
                persistence::yield_to_writers(txn);
                txn.exec_params("SELECT alttech.ensure_all_partitions($1, 7)",
                                static_cast<int>(backfill_days) + 1);
            });
            if (partitions) { ok = true; break; }
            last = partitions.error();
            log::warn("feature_partition_ensure_deferred",
                      {{"attempt", attempt},
                       {"of", kAttempts},
                       {"message", last.message}});
            if (ShutdownToken::instance().stop_requested()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(500 * attempt));
        }
        if (!ok) {
            // Fatal only when a write would actually fail. The partitions for
            // today and tomorrow are what the live pipeline needs this minute;
            // when every parent has them, the ensure was blocked on creating
            // the far end of the horizon (AltML's FDW cursor holds these
            // parents for minutes at a time), and refusing to start over that
            // is what turned a lock timeout into a 13-restart loop on
            // 2026-09-12 and again on 2026-09-18. The live loop below retries
            // every minute.
            auto covered = pool.with_txn([&](pqxx::work& txn) {
                const auto r = txn.exec(
                    "SELECT count(*) FROM unnest(alttech.partitioned_parents()) AS p "
                    "WHERE NOT EXISTS (SELECT 1 FROM alttech.partition_bounds(p::regclass) b "
                    "                  WHERE b.upper_bound > now() + interval '1 day')");
                if (r[0][0].as<int64_t>() != 0) {
                    throw std::runtime_error(std::to_string(r[0][0].as<int64_t>()) +
                                             " parent(s) lack a partition for tomorrow");
                }
            });
            if (!covered) {
                log::error("feature_partition_ensure_failed",
                           {{"message", last.message}, {"coverage", covered.error().message}});
                return fail_start(last);
            }
            log::error("feature_partition_ensure_deferred_at_start",
                       {{"message", last.message},
                        {"note", "today and tomorrow are covered; the live loop retries every minute"}});
            partition_ensure_deferred_at_start = true;
        }
    }

    auto set_id = ensure_core_feature_set(pool);
    if (!set_id) {
        log::error("feature_set_init_failed", {{"message", set_id.error().message}});
        return fail_start(set_id.error());
    }
    log::info("feature_set_ready", {{"feature_set_id", *set_id},
                                    {"features", talib_features::FeatureEngine::feature_count()},
                                    {"schema_hash", talib_features::core_schema_fingerprint().substr(0, 12)}});

    FeatureService service(service_config, pool, nats, *set_id);
    if (auto r = service.init_shards(); !r) {
        log::error("shard_init_failed", {{"message", r.error().message}});
        return fail_start(r.error());
    }

    // Bind synchronously. Readiness must mean that this module can actually
    // consume its durable, not merely that its worker threads were constructed.
    auto consumer = nats.pull_consumer(bar_stream, bar_consumer);
    if (!consumer) {
        log::error("feature_consumer_bind_failed",
                   {{"message", consumer.error().message},
                    {"stream", bar_stream},
                    {"durable", bar_consumer}});
        return fail_start(consumer.error());
    }

    auto& shutdown = ShutdownToken::instance();
    std::mutex failure_mu;
    std::optional<Error> runtime_error;
    const auto report_runtime_failure = [&](Error error) {
        bool first = false;
        {
            std::lock_guard lock(failure_mu);
            if (!runtime_error) {
                runtime_error = error;
                first = true;
            }
        }
        if (!first) return;
        log::error("feature_module_failed", {{"message", error.message}});
        shutdown.request_stop();
        supervisor.mark_failed(kModuleName, error);
    };

    std::vector<std::thread> pool_threads;
    std::optional<std::thread> live_thread;
    try {
        for (int i = 0; i < workers; ++i) {
            pool_threads.emplace_back([&service, &report_runtime_failure] {
                try {
                    service.worker_loop();
                } catch (const std::exception& exception) {
                    report_runtime_failure(
                        Error{ErrorCode::internal_error,
                              std::string("feature worker terminated: ") + exception.what()});
                } catch (...) {
                    report_runtime_failure(
                        Error{ErrorCode::internal_error,
                              "feature worker terminated with a non-standard exception"});
                }
            });
        }

        // Live consumer thread: durable pull on the configured external stream.
        live_thread.emplace([&, consumer = std::move(*consumer)]() mutable {
          int64_t retry_count = 0;
          int64_t last_retry_log_ms = 0;
          try {
        while (!shutdown.stop_requested()) {
            // Beacon the RPO-critical ingest path. Generous window: a legitimate
            // fetch+process cycle is sub-second, but a single slow-but-not-hung
            // query may run up to statement_timeout before the DB bound trips.
            supervisor.heartbeat(kModuleName, 300'000);
            auto msgs = consumer.fetch(128, 1500);
            if (!msgs) {
                if (shutdown.stop_requested()) break;
                if (!msgs.error().retryable) {
                    report_runtime_failure(msgs.error());
                    return;
                }
                log::warn("feature_consumer_fetch_retry",
                          {{"message", msgs.error().message}});
                if (shutdown.wait_for(std::chrono::seconds(1))) break;
                continue;
            }
            for (auto& msg : *msgs) {
                auto env = messaging::parse_envelope(msg.data());
                if (!env) {
                    log::warn("feature_bad_envelope", {{"message", env.error().message}});
                    (void)msg.term();
                    continue;
                }

                // JetStream's own redelivery counter: authoritative, and it
                // survives a restart of this consumer.
                const uint64_t deliveries = msg.delivery_count();

                Result<Delivery> handled = Delivery::ack;
                if (env->has_candle()) {
                    auto candle = messaging::candle_from_proto(env->candle());
                    if (!candle) {
                        handled = std::unexpected(candle.error());
                    } else if (candle->is_closed) {
                        handled = service.on_live_candle(*env, *candle, deliveries);
                    }
                } else if (env->has_candle_batch()) {
                    const auto& batch = env->candle_batch();
                    if (batch.candles_size() == 0) {
                        handled = fail(ErrorCode::validation_error, "empty candle batch");
                    } else {
                        std::vector<Candle> candles;
                        candles.reserve(static_cast<size_t>(batch.candles_size()));
                        for (const auto& proto_candle : batch.candles()) {
                            auto candle = messaging::candle_from_proto(proto_candle);
                            if (!candle) {
                                handled = std::unexpected(candle.error());
                                break;
                            }
                            candles.push_back(std::move(*candle));
                        }
                        if (handled && !candles.empty()) {
                            handled = service.on_batch(*env, candles.front().timeframe, candles, deliveries);
                        }
                    }
                } else {
                    handled = fail(ErrorCode::validation_error,
                                   "feature consumer received unsupported payload");
                }

                if (!handled) {
                    const auto code = handled.error().code;
                    if (code == ErrorCode::validation_error || code == ErrorCode::parse_error ||
                        code == ErrorCode::invalid_argument) {
                        log::warn("feature_message_rejected",
                                  {{"message", handled.error().message}});
                        (void)msg.term();
                    } else {
                        // One line per NAK floods the journal: a sustained
                        // failure produced 108 000 lines in ten minutes, which
                        // buries every other signal. Report the first, then a
                        // periodic summary.
                        ++retry_count;
                        const int64_t now = timeutil::now_ms();
                        if (retry_count == 1 || now - last_retry_log_ms >= 30'000) {
                            log::warn("feature_message_retry",
                                      {{"message", handled.error().message},
                                       {"suppressed_since_last", retry_count - 1}});
                            last_retry_log_ms = now;
                            retry_count = 0;
                        }
                        (void)msg.nak(1000);
                    }
                    continue;
                }
                switch (*handled) {
                case Delivery::retry: (void)msg.nak(1000); break;
                // Terminal: redelivering it again would only rebuild the same
                // disagreement and keep occupying the ack-pending budget.
                case Delivery::divergent: (void)msg.term(); break;
                case Delivery::ack: (void)msg.ack(); break;
                }
            }
        }
          } catch (const std::exception& exception) {
              report_runtime_failure(
                  Error{ErrorCode::internal_error,
                        std::string("feature consumer terminated: ") + exception.what()});
          } catch (...) {
              report_runtime_failure(
                  Error{ErrorCode::internal_error,
                        "feature consumer terminated with a non-standard exception"});
          }
        });
    } catch (const std::system_error& exception) {
        report_runtime_failure(
            Error{ErrorCode::internal_error,
                  std::string("failed to start feature module thread: ") + exception.what()});
        if (live_thread && live_thread->joinable()) live_thread->join();
        for (auto& thread : pool_threads) {
            if (thread.joinable()) thread.join();
        }
        std::lock_guard lock(failure_mu);
        return std::unexpected(*runtime_error);
    }

    supervisor.mark_ready(kModuleName);

    // Reference-venue feature vectors. Runs alongside the primary-venue engine
    // and degrades independently: a reference venue outage never stops it.
    std::thread venue_thread(venuelite::run_venue_sweep, std::ref(pool), std::ref(nats),
                             venuelite::store::VenueOutput::feature, service_config);

    int64_t last_stats = timeutil::now_ms();
    // Partitions are ensured at start-up, three days ahead -- and nothing used
    // to advance them afterwards. `ensure_all_partitions` had exactly two
    // callers: the start-up path above, and the CLI that the (disabled)
    // alttech-maintenance.timer runs. A service that stays up past its own
    // three-day horizon therefore loses EVERY feature write at the next day
    // boundary while systemd still reports it active; on 2026-09-04 that
    // starved AltStrategy and AltSignalQuality for four hours, with the only
    // visible symptom being that AltStrategy's `context_not_built` lines
    // stopped appearing at all. AltSpotter has advanced its own partitions
    // hourly from its live loop all along (components/retention_manager.cpp);
    // this is the same thing for the one service that lacked it.
    // Partition advance: hourly when it succeeds, EVERY MINUTE while it is
    // deferred, seven days ahead.
    //
    // The hourly single attempt failed 72 times in a row on 2026-09-14..16:
    // AltML's archive backfill holds an AccessShare on these parents for most
    // of every hour (one FDW cursor per venue/grid, minutes each, 60 s idle
    // between passes), and CREATE TABLE ... PARTITION OF needs AccessExclusive,
    // so a 750 ms lock timeout tried once an hour at a fixed minute almost
    // never lands in the idle gap. The three-day horizon ran out at 00:00 on
    // 2026-09-17; every feature write then failed, PostgreSQL logged each
    // failing statement in full, the log filled the disk in 4.6 hours and the
    // database went down for 40 hours. Nothing had exited, systemd reported
    // every unit active, and the warning had been in the journal 72 times.
    //
    // Two changes, both about WHEN, not WHAT: a deferred attempt is retried a
    // minute later (60 attempts an hour reliably find the idle gap), and the
    // horizon is a week, so even a whole day of lost attempts costs nothing.
    constexpr int kPartitionDaysAhead = 7;
    constexpr int64_t kAdvanceIntervalMs = 3600'000;
    constexpr int64_t kRetryIntervalMs = 60'000;
    int64_t next_partitions_at = partition_ensure_deferred_at_start
                                     ? timeutil::now_ms() + kRetryIntervalMs
                                     : timeutil::now_ms() + kAdvanceIntervalMs;
    int deferred_streak = 0;
    while (!shutdown.wait_for(std::chrono::seconds(5))) {
        // Cheap on every tick: the per-shard check is two integer comparisons
        // and the queue merges duplicates, so this is a no-op until a reference
        // venue's bar actually closes.
        service.enqueue_reference_catchup();
        if (timeutil::now_ms() - last_stats >= 60000) {
            service.log_stats();
            last_stats = timeutil::now_ms();
        }
        if (timeutil::now_ms() >= next_partitions_at) {
            // Deferred, never fatal: there is a live pipeline to protect, and a
            // lock timeout against the feature writers or AltML's FDW cursor is
            // transient. But transient is only true if the retry is soon.
            auto ensured = pool.with_txn([&](pqxx::work& txn) {
                persistence::yield_to_writers(txn);
                txn.exec_params("SELECT alttech.ensure_all_partitions($1, $2)",
                                static_cast<int>(backfill_days) + 1, kPartitionDaysAhead);
            });
            if (ensured) {
                if (deferred_streak > 0) {
                    log::info("partition_advance_recovered", {{"after_deferrals", deferred_streak}});
                }
                deferred_streak = 0;
                next_partitions_at = timeutil::now_ms() + kAdvanceIntervalMs;
                log::info("partitions_advanced", {{"days_ahead", kPartitionDaysAhead}});
            } else {
                ++deferred_streak;
                next_partitions_at = timeutil::now_ms() + kRetryIntervalMs;
                // Escalates to an error after an hour of deferrals: a warning
                // that repeats 72 times is what an outage looked like last time.
                const auto fields = nlohmann::json{{"message", ensured.error().message},
                                                   {"deferred_streak", deferred_streak},
                                                   {"retry_in_sec", 60}};
                if (deferred_streak >= 60) {
                    log::error("partition_advance_stalled", fields);
                } else {
                    log::warn("partition_advance_deferred", fields);
                }
            }
        }
    }

    if (venue_thread.joinable()) venue_thread.join();
    if (live_thread && live_thread->joinable()) live_thread->join();
    for (auto& thread : pool_threads) {
        if (thread.joinable()) thread.join();
    }
    log::info("feature_module_stopped", {});

    std::lock_guard lock(failure_mu);
    if (runtime_error) return std::unexpected(*runtime_error);
    return {};
}

} // namespace alttech::modules
