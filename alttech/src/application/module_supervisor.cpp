#include "alttech/application/module_supervisor.hpp"

#include "alttech/common/shutdown.hpp"
#include "alttech/logging/logger.hpp"
#include "alttech/time/time.hpp"

#include <chrono>

namespace alttech::application {

ModuleSupervisor::ModuleSupervisor(std::size_t expected_modules)
    : expected_modules_(expected_modules) {}

void ModuleSupervisor::mark_ready(std::string_view module) {
    {
        std::lock_guard lock(mutex_);
        ready_modules_.emplace(module);
    }
    changed_.notify_all();
    alttech::log::info("analytics_module_ready", {{"module", std::string(module)}});
}

void ModuleSupervisor::mark_failed(std::string_view module, const alttech::Error& error) {
    bool first_failure = false;
    {
        std::lock_guard lock(mutex_);
        if (!failure_) {
            failure_ = ModuleFailure{std::string(module), error};
            first_failure = true;
        }
    }
    changed_.notify_all();
    if (first_failure) {
        alttech::log::critical("analytics_module_failed",
                           {{"module", std::string(module)}, {"message", error.message}});
        alttech::ShutdownToken::instance().request_stop();
    }
}

bool ModuleSupervisor::wait_until_started() {
    std::unique_lock lock(mutex_);
    while (ready_modules_.size() < expected_modules_ && !failure_) {
        if (alttech::ShutdownToken::instance().stop_requested()) return false;
        changed_.wait_for(lock, std::chrono::milliseconds(250));
    }
    return !failure_ && ready_modules_.size() == expected_modules_;
}

std::optional<ModuleFailure> ModuleSupervisor::failure() const {
    std::lock_guard lock(mutex_);
    return failure_;
}

std::size_t ModuleSupervisor::ready_count() const {
    std::lock_guard lock(mutex_);
    return ready_modules_.size();
}

void ModuleSupervisor::heartbeat(std::string_view module, int64_t valid_for_ms) {
    if (valid_for_ms <= 0) return;
    const int64_t deadline = alttech::timeutil::now_ms() + valid_for_ms;
    std::lock_guard lock(mutex_);
    liveness_deadline_ms_[std::string(module)] = deadline;
}

bool ModuleSupervisor::all_alive() const {
    const int64_t now = alttech::timeutil::now_ms();
    std::lock_guard lock(mutex_);
    for (const auto& [module, deadline] : liveness_deadline_ms_) {
        if (now > deadline) return false;
    }
    return true;
}

std::vector<std::pair<std::string, int64_t>> ModuleSupervisor::stalled_modules() const {
    const int64_t now = alttech::timeutil::now_ms();
    std::vector<std::pair<std::string, int64_t>> out;
    std::lock_guard lock(mutex_);
    for (const auto& [module, deadline] : liveness_deadline_ms_) {
        if (now > deadline) out.emplace_back(module, now - deadline);
    }
    return out;
}

} // namespace alttech::application
