// AltTech common runtime implementation.
#include "alttech/common/readiness.hpp"

#include <systemd/sd-daemon.h>

#include <string>

namespace alttech {

void notify_ready() { sd_notify(0, "READY=1"); }

void notify_stopping() { sd_notify(0, "STOPPING=1"); }

void notify_status(const char* status) {
    const std::string line = std::string("STATUS=") + status;
    sd_notify(0, line.c_str());
}

void notify_watchdog() { sd_notify(0, "WATCHDOG=1"); }

uint64_t watchdog_interval_usec() {
    uint64_t usec = 0;
    // >0 return means the watchdog is enabled for this process; usec is filled in.
    return sd_watchdog_enabled(0, &usec) > 0 ? usec : 0;
}

} // namespace alttech
