#include "altml/components/model_registry.hpp"

#include "altml/core/codec.hpp"
#include "altml/core/logger.hpp"
#include "altml/core/readiness.hpp"
#include "altml/core/shutdown.hpp"
#include "altml/core/time.hpp"

#include <chrono>
#include <utility>
#include <vector>

namespace altml {

ModelRegistryComponent::ModelRegistryComponent(const config::RuntimeConfig& rt,
                                               messaging::NatsClient& nats)
    : rt_(rt), nats_(nats) {}

Result<void> ModelRegistryComponent::init() {
    const YAML::Node section = rt_.root["model_registry"];
    poll_sec_ = config::opt_int(section, "poll_interval_sec", 30);
    pool_.emplace(rt_.common.postgres_dsn,
                  static_cast<size_t>(config::opt_int(section, "postgres_pool_size", 1)));
    if (auto probe = pool_->with_txn([](pqxx::work& txn) { txn.exec1("SELECT 1"); }); !probe) {
        return fail(ErrorCode::database_error,
                    "postgres unavailable: " + probe.error().message);
    }
    set_component_status(name(), "watching model registry");
    return {};
}

void ModelRegistryComponent::run() {
    auto& shutdown = ShutdownToken::instance();
    while (!shutdown.stop_requested()) {
        std::vector<std::pair<std::string, std::string>> current;
        auto res = pool_->with_txn([&](pqxx::work& txn) {
            const auto rows = txn.exec("SELECT model_id, status FROM altml.models");
            for (const auto& r : rows) {
                current.emplace_back(r[0].as<std::string>(), r[1].as<std::string>());
            }
        });
        if (res) {
            for (const auto& [id, status] : current) {
                const auto it = known_.find(id);
                if (it != known_.end() && it->second == status) continue;
                const bool is_new = it == known_.end();
                known_[id] = status;
                if (first_pass_) continue; // baseline snapshot without event spam
                messaging::EnvelopeMeta meta;
                meta.event_type = "model." + status;
                meta.source_service = "altml";
                meta.message_id = "model:" + id + ":" + status;
                auto env = messaging::make_envelope(meta);
                const std::string subject =
                    "evt.model." + std::string(is_new ? "registered" : "transition");
                if (auto published =
                        nats_.js_publish(subject, messaging::serialize(env), env.message_id());
                    !published) {
                    log::error("model_transition_publish_failed",
                               {{"model_id", id}, {"status", status},
                                {"message", published.error().message}});
                }
                log::info("model_transition", {{"model_id", id},
                                               {"status", status},
                                               {"new", is_new}});
            }
            first_pass_ = false;
        } else {
            log::warn("registry_poll_failed", {{"message", res.error().message}});
        }
        if (shutdown.wait_for(std::chrono::seconds(poll_sec_))) break;
    }
}

} // namespace altml
