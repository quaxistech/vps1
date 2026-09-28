#include "altsignalquality/components/model_registry.hpp"

#include "altsignalquality/core/codec.hpp"
#include "altsignalquality/core/logger.hpp"
#include "altsignalquality/core/nats_subjects.hpp"
#include "altsignalquality/core/pg.hpp"
#include "altsignalquality/core/readiness.hpp"
#include "altsignalquality/core/shutdown.hpp"
#include "altsignalquality/core/time.hpp"
#include "altsignalquality/data/store.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <utility>

namespace altsignalquality {

struct ModelRegistryComponent::Impl {
    int64_t poll_sec = 30;
    std::optional<persistence::PgPool> pool;
    std::optional<data::Store> store;
    std::map<std::string, std::string> known;  // model_id -> status
    bool first_pass = true;
};

ModelRegistryComponent::ModelRegistryComponent(const config::RuntimeConfig& rt,
                                               messaging::NatsClient& nats)
    : rt_(rt), nats_(nats), impl_(std::make_unique<Impl>()) {}

ModelRegistryComponent::~ModelRegistryComponent() = default;

Result<void> ModelRegistryComponent::init() {
    const YAML::Node section = rt_.root["model_registry"];
    impl_->poll_sec = config::opt_int(section, "poll_interval_sec", 30);
    if (impl_->poll_sec < 1) {
        return fail(ErrorCode::config_error, "model_registry.poll_interval_sec must be >= 1");
    }
    impl_->pool.emplace(rt_.common.postgres_dsn,
                        static_cast<size_t>(config::opt_int(section, "postgres_pool_size", 1)));
    if (auto probe = impl_->pool->with_txn([](pqxx::work& txn) { txn.exec1("SELECT 1"); });
        !probe) {
        return fail(ErrorCode::database_error, "postgres unavailable: " + probe.error().message);
    }
    impl_->store.emplace(*impl_->pool, rt_.wiring.altstrategy_schema,
                         rt_.wiring.max_stop_distance_bps);
    set_component_status(name(), "watching model registry");
    return {};
}

void ModelRegistryComponent::run() {
    auto& shutdown = ShutdownToken::instance();
    while (!shutdown.stop_requested()) {
        auto current = impl_->store->list_model_statuses();
        if (current) {
            for (const auto& [id, status] : *current) {
                const auto it = impl_->known.find(id);
                if (it != impl_->known.end() && it->second == status) continue;
                const bool is_new = it == impl_->known.end();
                impl_->known[id] = status;
                // The first pass is a baseline snapshot: republishing every
                // historical model on every restart would be noise, not news.
                if (impl_->first_pass) continue;

                messaging::EnvelopeMeta meta;
                meta.message_id = "model:" + id + ":" + status;
                meta.event_type = "model." + status;
                auto env = messaging::make_envelope(meta);
                auto* ev = env.mutable_model_event();
                ev->set_model_id(id);
                ev->set_status(status);
                ev->set_event(is_new ? "registered" : "transition");
                ev->set_created_at_ms(timeutil::now_ms());
                ev->set_detail_json(nlohmann::json({{"objective", rt_.wiring.objective}}).dump());
                const std::string subject = nats_subjects::model_event(
                    rt_.wiring, is_new ? "registered" : "transition");
                if (auto r = nats_.js_publish(subject, messaging::serialize(env), meta.message_id);
                    !r) {
                    log::error("model_event_publish_failed",
                               {{"model_id", id}, {"status", status},
                                {"message", r.error().message}});
                }
                log::info("model_transition",
                          {{"model_id", id}, {"status", status}, {"new", is_new}});
            }
            impl_->first_pass = false;
        } else {
            log::warn("registry_poll_failed", {{"message", current.error().message}});
        }
        if (shutdown.wait_for(std::chrono::seconds(impl_->poll_sec))) break;
    }
}

} // namespace altsignalquality
