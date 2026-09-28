#pragma once
// Internal AltTech configuration API.

#include "alttech/config/config.hpp"

#include <string>

namespace alttech::config {

// Shared service entry boilerplate:
//   <service> --config /etc/alttech/<name>.yaml [--common /etc/alttech/common.yaml]
// Loads both configs, initializes JSON logging and signal handlers.
struct ServiceContext {
    CommonConfig common;
    YAML::Node service; // raw service YAML (already env-expanded)
    std::string service_name;
};

Result<ServiceContext> bootstrap_service(const std::string& service_name, int argc, char** argv);

} // namespace alttech::config
