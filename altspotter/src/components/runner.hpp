#pragma once

#include <string>
#include <vector>

namespace altspotter {

// `altspotter run`: load the shared common block + unified service config, start
// the selected market-data components as supervised threads, signal systemd
// readiness, and block until SIGINT/SIGTERM. Returns a process exit code.
//
// `only` selects components by their config-section name (e.g. "storage_writer");
// an empty list runs all components. Unknown names are a configuration error.
int run_service(const std::string& config_path, const std::string& common_path,
                const std::vector<std::string>& only = {});

}  // namespace altspotter
