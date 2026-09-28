// alttech-config-check: validates AltTech YAML without printing secrets.
//
//   alttech-config-check --dir /etc/alttech
//   alttech-config-check --file <path> --kind <common|service>

#include "alttech/config/analytics_config.hpp"
#include "alttech/config/config.hpp"
#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using alttech::config::load_yaml;

namespace {

struct CheckResult {
    std::string file;
    bool ok = true;
    std::vector<std::string> errors;
};

void require(CheckResult& result, bool condition, const std::string& message) {
    if (!condition) {
        result.ok = false;
        result.errors.push_back(message);
    }
}

CheckResult check_common(const std::string& path) {
    CheckResult result{.file = path, .ok = true, .errors = {}};
    const auto config = alttech::config::load_common(path);
    require(result, config.has_value(), config ? "" : config.error().message);
    return result;
}

CheckResult check_service(const std::string& path) {
    CheckResult result{.file = path, .ok = true, .errors = {}};
    const auto document = load_yaml(path);
    if (!document) {
        require(result, false, document.error().message);
        return result;
    }

    const auto parsed = alttech::config::parse_analytics_config(*document);
    require(result, parsed.has_value(), parsed ? "" : parsed.error().message);

    return result;
}

void print_usage(std::ostream& output) {
    output << "usage: alttech-config-check --dir /etc/alttech | "
              "--file F --kind K\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string dir;
    std::string file;
    std::string kind;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--dir" && i + 1 < argc) {
            dir = argv[++i];
        } else if (argument == "--file" && i + 1 < argc) {
            file = argv[++i];
        } else if (argument == "--kind" && i + 1 < argc) {
            kind = argv[++i];
        } else if (argument == "--help" || argument == "-h") {
            print_usage(std::cout);
            return 0;
        } else {
            std::cerr << "unknown or incomplete argument: " << argument << "\n";
            print_usage(std::cerr);
            return 2;
        }
    }

    std::vector<CheckResult> results;
    if (!file.empty()) {
        if (kind.empty()) {
            std::cerr << "--kind is required with --file\n";
            return 2;
        }
        if (kind == "common") {
            results.push_back(check_common(file));
        } else if (kind == "service") {
            results.push_back(check_service(file));
        } else {
            std::cerr << "unknown config kind: " << kind << "\n";
            return 2;
        }
    } else if (!dir.empty()) {
        const std::vector<std::pair<std::string, std::string>> configs = {
            {"common.yaml", "common"},
            {"alttech.yaml", "service"},
        };
        for (const auto& [name, config_kind] : configs) {
            const fs::path path = fs::path(dir) / name;
            results.push_back(config_kind == "common" ? check_common(path.string())
                                                       : check_service(path.string()));
        }
    } else {
        print_usage(std::cerr);
        return 2;
    }

    bool all_ok = true;
    for (const auto& result : results) {
        if (result.ok) {
            std::cout << "OK    " << result.file << "\n";
            continue;
        }
        all_ok = false;
        std::cout << "ERROR " << result.file << "\n";
        for (const auto& error : result.errors) {
            if (!error.empty()) std::cout << "      - " << error << "\n";
        }
    }
    return all_ok ? 0 : 1;
}
