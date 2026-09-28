#include "altsignalgateway/core/readiness.hpp"

#include <systemd/sd-daemon.h>

#include <map>
#include <mutex>
#include <string>

namespace altsignalgateway {

void notify_ready() { sd_notify(0, "READY=1"); }

void notify_stopping() { sd_notify(0, "STOPPING=1"); }

int64_t watchdog_interval_ms() {
    uint64_t usec = 0;
    // sd_watchdog_enabled also validates WATCHDOG_PID against this process.
    if (sd_watchdog_enabled(0, &usec) > 0 && usec > 0) {
        return static_cast<int64_t>(usec / 2000); // ping at half the budget
    }
    return 0;
}

void notify_watchdog() { sd_notify(0, "WATCHDOG=1"); }

void notify_status(const char* status) {
    const std::string line = std::string("STATUS=") + status;
    sd_notify(0, line.c_str());
}

namespace {
std::mutex g_status_mu;
std::map<std::string, std::string, std::less<>> g_component_status;
} // namespace

void set_component_status(std::string_view component, std::string_view status) {
    std::string merged;
    {
        std::lock_guard lock(g_status_mu);
        auto it = g_component_status.find(component);
        if (it != g_component_status.end() && it->second == status) return;
        g_component_status[std::string(component)] = std::string(status);
        for (const auto& [name, value] : g_component_status) {
            if (!merged.empty()) merged += ' ';
            merged += name;
            merged += '=';
            merged += value;
        }
    }
    notify_status(merged.c_str());
}

} // namespace altsignalgateway
