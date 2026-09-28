#pragma once
// Internal AltTech runtime API.

#include <cstdint>

namespace alttech {

// systemd readiness integration (Type=notify). No-ops when NOTIFY_SOCKET is unset,
// so local runs behave identically.
void notify_ready();
void notify_stopping();
void notify_status(const char* status);

// Liveness heartbeat for a Type=notify service with WatchdogSec set: sends
// WATCHDOG=1. systemd force-restarts the process if these stop arriving, which
// converts a silent hang (not just a crash) into an automatic recovery.
void notify_watchdog();

// Watchdog interval in microseconds if the manager enabled it for this process
// (WatchdogSec set and WATCHDOG_USEC/WATCHDOG_PID matched), else 0. Ping at no
// more than half this interval.
uint64_t watchdog_interval_usec();

} // namespace alttech
