#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace altml::log {

enum class Level : int { trace = 0, debug = 1, info = 2, warn = 3, error = 4, critical = 5 };

// Structured JSON logging. Under systemd it is sent directly to journald with
// the native severity; elsewhere it falls back to stdout. One line per event:
// {"timestamp":"...","level":"info","service":"...","instance":"...","event":"...",...}
void init(std::string service, std::string instance, std::string_view level);

// Component identity for the unified service: every thread tags its records
// (JSON field "component", journald field ALTML_COMPONENT) so one process can
// host several components without losing per-component filtering.
void set_thread_component(std::string component);

Level parse_level(std::string_view s);

void write(Level level, std::string_view event, nlohmann::json fields);

inline void trace(std::string_view event, nlohmann::json fields = {}) {
    write(Level::trace, event, std::move(fields));
}
inline void debug(std::string_view event, nlohmann::json fields = {}) {
    write(Level::debug, event, std::move(fields));
}
inline void info(std::string_view event, nlohmann::json fields = {}) {
    write(Level::info, event, std::move(fields));
}
inline void warn(std::string_view event, nlohmann::json fields = {}) {
    write(Level::warn, event, std::move(fields));
}
inline void error(std::string_view event, nlohmann::json fields = {}) {
    write(Level::error, event, std::move(fields));
}
inline void critical(std::string_view event, nlohmann::json fields = {}) {
    write(Level::critical, event, std::move(fields));
}

// Rate-limited logging for hot paths: at most one record per key per interval;
// suppressed count is attached to the next emitted record.
class RateLimited {
public:
    explicit RateLimited(std::chrono::milliseconds interval) : interval_(interval) {}

    void log(Level level, const std::string& key, std::string_view event, nlohmann::json fields);

private:
    struct Entry {
        std::chrono::steady_clock::time_point last{};
        uint64_t suppressed = 0;
    };
    std::mutex mu_;
    std::chrono::milliseconds interval_;
    std::unordered_map<std::string, Entry> entries_;
};

} // namespace altml::log
