#pragma once

#include <cstdint>
#include <string_view>

namespace altmacro {

// systemd readiness integration (Type=notify). No-ops when NOTIFY_SOCKET is unset,
// so local runs behave identically.
void notify_ready();
void notify_stopping();
void notify_status(const char* status);

// systemd watchdog (WatchdogSec=). Returns the ping interval in milliseconds
// (half the WATCHDOG_USEC budget) or 0 when no watchdog is armed.
int64_t watchdog_interval_ms();
void notify_watchdog();

// Aggregated status for the unified service: each component reports its own
// line; the merged "a=x b=y" string is forwarded as the single per-process
// systemd STATUS (sd_notify has no per-thread notion).
void set_component_status(std::string_view component, std::string_view status);

} // namespace altmacro
