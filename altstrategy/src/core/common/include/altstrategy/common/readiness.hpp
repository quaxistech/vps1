#pragma once

namespace altstrategy {

// systemd readiness integration (Type=notify). No-ops when NOTIFY_SOCKET is unset,
// so local runs behave identically.
void notify_ready();
void notify_stopping();

// Liveness ping for systemd's WatchdogSec. Call from the main loop only.
void notify_watchdog();
void notify_status(const char* status);

} // namespace altstrategy
