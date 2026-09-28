#include "altsignalquality/core/time.hpp"

#include <cinttypes>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace altsignalquality::timeutil {

int64_t now_ms() {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

std::string to_iso8601(int64_t ms) {
    const time_t secs = static_cast<time_t>(ms >= 0 ? ms / 1000 : (ms - 999) / 1000);
    const int millis = static_cast<int>(ms - static_cast<int64_t>(secs) * 1000);
    std::tm tm{};
    if (gmtime_r(&secs, &tm) == nullptr) return {};
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0')
        << std::setw(3) << millis << 'Z';
    return out.str();
}

std::optional<int64_t> parse_iso8601(std::string_view s) {
    int year = 0, mon = 0, day = 0, hour = 0, min = 0;
    double sec = 0.0;
    // Accept "YYYY-MM-DD HH:MM:SS[.fff][+00|Z]" and "YYYY-MM-DDTHH:MM:SS[.fff]Z".
    char buf[64];
    if (s.size() >= sizeof(buf)) return std::nullopt;
    for (size_t i = 0; i < s.size(); ++i) buf[i] = (s[i] == 'T') ? ' ' : s[i];
    buf[s.size()] = '\0';
    if (std::sscanf(buf, "%d-%d-%d %d:%d:%lf", &year, &mon, &day, &hour, &min, &sec) != 6) {
        return std::nullopt;
    }
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = mon - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = 0;
    const time_t base = timegm(&tm);
    if (base == static_cast<time_t>(-1)) return std::nullopt;
    return static_cast<int64_t>(base) * 1000 + static_cast<int64_t>(sec * 1000.0 + 0.5);
}

std::string month_key(int64_t ms) {
    const time_t secs = static_cast<time_t>(ms / 1000);
    std::tm tm{};
    if (gmtime_r(&secs, &tm) == nullptr) return {};
    std::ostringstream out;
    out << std::put_time(&tm, "%Y%m");
    return out.str();
}

} // namespace altsignalquality::timeutil
