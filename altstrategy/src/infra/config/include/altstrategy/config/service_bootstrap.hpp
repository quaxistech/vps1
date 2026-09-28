#pragma once

#include "altstrategy/config/config.hpp"

#include <string>

namespace altstrategy::config {

// Shared service entry boilerplate:
//   <service> --config /etc/altstrategy/<name>.yaml [--common /etc/altstrategy/common.yaml]
// Loads both configs, initializes JSON logging and signal handlers.
struct ServiceContext {
    CommonConfig common;
    YAML::Node common_yaml; // raw common YAML (for the project: adapter)
    YAML::Node service;    // raw service YAML (already env-expanded)
    std::string service_name;
};

Result<ServiceContext> bootstrap_service(const std::string& service_name, int argc, char** argv);

} // namespace altstrategy::config
