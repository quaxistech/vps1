#include "core/readiness.hpp"

#include <systemd/sd-daemon.h>

#include <string>

namespace altspotter {

void notify_ready() { sd_notify(0, "READY=1"); }

void notify_stopping() { sd_notify(0, "STOPPING=1"); }

void notify_watchdog() { sd_notify(0, "WATCHDOG=1"); }

uint64_t watchdog_interval_usec() {
    uint64_t usec = 0;
    // Returns > 0 only in the process systemd started with WatchdogSec set.
    return sd_watchdog_enabled(0, &usec) > 0 ? usec : 0;
}

void notify_status(const char* status) {
    const std::string line = std::string("STATUS=") + status;
    sd_notify(0, line.c_str());
}

} // namespace altspotter
