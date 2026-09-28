#include "altstrategy/logging/logger.hpp"

#include "altstrategy/time/time.hpp"

#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>
#include <systemd/sd-journal.h>

#include <atomic>
#include <cstdlib>

namespace altstrategy::log {

namespace {

struct State {
    std::string service = "unknown";
    std::string instance = "unknown";
    std::atomic<int> min_level{static_cast<int>(Level::info)};
    std::shared_ptr<spdlog::logger> sink;
    bool direct_journal = false;
};

State& state() {
    static State s;
    return s;
}

constexpr std::string_view level_name(Level l) noexcept {
    switch (l) {
        case Level::trace: return "trace";
        case Level::debug: return "debug";
        case Level::info: return "info";
        case Level::warn: return "warn";
        case Level::error: return "error";
        case Level::critical: return "critical";
    }
    return "?";
}

constexpr int journal_priority(Level level) noexcept {
    switch (level) {
        case Level::trace:
        case Level::debug: return 7;
        case Level::info: return 6;
        case Level::warn: return 4;
        case Level::error: return 3;
        case Level::critical: return 2;
    }
    return 5;
}

constexpr spdlog::level::level_enum spd_level(Level level) noexcept {
    switch (level) {
        case Level::trace: return spdlog::level::trace;
        case Level::debug: return spdlog::level::debug;
        case Level::info: return spdlog::level::info;
        case Level::warn: return spdlog::level::warn;
        case Level::error: return spdlog::level::err;
        case Level::critical: return spdlog::level::critical;
    }
    return spdlog::level::info;
}

} // namespace

Level parse_level(std::string_view s) {
    if (s == "trace") return Level::trace;
    if (s == "debug") return Level::debug;
    if (s == "warn" || s == "warning") return Level::warn;
    if (s == "error") return Level::error;
    if (s == "critical") return Level::critical;
    return Level::info;
}

void init(std::string service, std::string instance, std::string_view level) {
    auto& s = state();
    s.service = std::move(service);
    s.instance = std::move(instance);
    s.min_level.store(static_cast<int>(parse_level(level)));
    s.direct_journal = std::getenv("JOURNAL_STREAM") != nullptr;
    if (!s.sink) {
        s.sink = spdlog::stdout_logger_mt("altstrategy_json");
        s.sink->set_pattern("%v"); // raw JSON line
        s.sink->set_level(spdlog::level::trace);
        s.sink->flush_on(spdlog::level::info);
    }
}

void write(Level level, std::string_view event, nlohmann::json fields) {
    auto& s = state();
    if (static_cast<int>(level) < s.min_level.load(std::memory_order_relaxed)) return;

    nlohmann::json record;
    record["timestamp"] = timeutil::to_iso8601(timeutil::now_ms());
    record["level"] = level_name(level);
    record["service"] = s.service;
    record["instance"] = s.instance;
    record["event"] = std::string(event);
    if (fields.is_object()) {
        for (auto& [k, v] : fields.items()) record[k] = std::move(v);
    }

    const std::string line = record.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    if (s.direct_journal &&
        sd_journal_send("MESSAGE=%s", line.c_str(), "PRIORITY=%i", journal_priority(level),
                        "SYSLOG_IDENTIFIER=%s", s.service.c_str(), "ALTSTRATEGY_INSTANCE=%s",
                        s.instance.c_str(), static_cast<const char*>(nullptr)) >= 0) {
        return;
    }
    if (s.sink) {
        s.sink->log(spd_level(level), line);
    } else {
        std::fputs(line.c_str(), stdout);
        std::fputc('\n', stdout);
    }
}

void RateLimited::log(Level level, const std::string& key, std::string_view event,
                      nlohmann::json fields) {
    uint64_t suppressed = 0;
    {
        std::lock_guard lock(mu_);
        auto& e = entries_[key];
        const auto now = std::chrono::steady_clock::now();
        if (e.last.time_since_epoch().count() != 0 && now - e.last < interval_) {
            ++e.suppressed;
            return;
        }
        suppressed = e.suppressed;
        e.suppressed = 0;
        e.last = now;
    }
    if (suppressed > 0) fields["suppressed_count"] = suppressed;
    write(level, event, std::move(fields));
}

} // namespace altstrategy::log
