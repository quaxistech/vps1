#pragma once

#include <cstdint>
#include <sys/statvfs.h>

namespace altml::sysinfo {

// Free space, in whole gigabytes (10^9), on the filesystem holding `path`, as
// an unprivileged process sees it (f_bavail: the root-reserved blocks are not
// counted, which is the honest figure for a service that cannot use them).
// Returns -1 when the filesystem cannot be queried, and a caller treats -1 as
// "unknown -> do not block", the same rule the memory probe follows.
inline int64_t free_disk_gb(const char* path) {
    struct statvfs st{};
    if (::statvfs(path, &st) != 0) return -1;
    const unsigned long long bytes =
        static_cast<unsigned long long>(st.f_bavail) * static_cast<unsigned long long>(st.f_frsize);
    return static_cast<int64_t>(bytes / 1'000'000'000ULL);
}

} // namespace altml::sysinfo
