#pragma once

#include <cstdint>

namespace altspotter {

// systemd readiness integration (Type=notify). No-ops when NOTIFY_SOCKET is unset,
// so local runs behave identically.
void notify_ready();
void notify_stopping();
void notify_status(const char* status);

// systemd watchdog heartbeat (WATCHDOG=1). No-op when the unit sets no
// WatchdogSec, so behaviour is unchanged where it is not enabled.
void notify_watchdog();

// The unit's WatchdogSec in microseconds, or 0 when systemd did not set one.
// Read from WATCHDOG_USEC, which systemd exports only to the supervised
// process it applies to.
uint64_t watchdog_interval_usec();

} // namespace altspotter
