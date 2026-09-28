#include "altstrategy/common/readiness.hpp"

#include <systemd/sd-daemon.h>

#include <string>

namespace altstrategy {

void notify_ready() { sd_notify(0, "READY=1"); }

void notify_stopping() { sd_notify(0, "STOPPING=1"); }

// Liveness, sent from the loop that is supposed to be doing the work.
//
// The unit declares Type=notify and READY=1 was sent, but nothing ever sent
// WATCHDOG=1 and no WatchdogSec was configured -- so a wedged engine (a
// deadlocked handler, a NATS drain that never returns) would have kept the
// service "active" indefinitely with systemd none the wiser. Sending this from
// a dedicated thread would have been worse than not sending it: the heartbeat
// has to prove the main loop still runs, not that a timer still ticks.
void notify_watchdog() { sd_notify(0, "WATCHDOG=1"); }

void notify_status(const char* status) {
    const std::string line = std::string("STATUS=") + status;
    sd_notify(0, line.c_str());
}

} // namespace altstrategy
