#include "altmacro/cli/commands.hpp"
#include "altmacro/core/config.hpp"
#include "altmacro/core/logger.hpp"

#include <cstring>
#include <iostream>
#include <string>

namespace {

void usage() {
    std::cout <<
        "altmacro — external market and macro reference data for VPS1\n"
        "\n"
        "  altmacro run          --config PATH      run the service (systemd)\n"
        "  altmacro bootstrap    --config PATH      load history; safe to interrupt and repeat\n"
        "                        [--no-gap-scan]\n"
        "  altmacro inspect      --config PATH      report registry / coverage / providers\n"
        "                        [--what registry|coverage|providers|all]\n"
        "  altmacro nats-setup   --config PATH      create or update the JetStream stream\n"
        "  altmacro selftest                        offline invariants; no network, no database\n"
        "\n"
        "The database password is read from ALTMACRO_DB_PASSWORD. It is never read from\n"
        "configuration and never written to a log.\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    const std::string command = argv[1];

    if (command == "-h" || command == "--help" || command == "help") { usage(); return 0; }
    if (command == "selftest") {
        altmacro::log::init("altmacro", "cli", "info");
        return altmacro::cli::run_selftest();
    }

    std::string config_path;
    std::string what = "all";
    bool gap_scan = true;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) config_path = argv[++i];
        else if (arg == "--what" && i + 1 < argc) what = argv[++i];
        else if (arg == "--no-gap-scan") gap_scan = false;
        else { std::cerr << "unrecognised argument: " << arg << "\n"; usage(); return 2; }
    }
    if (config_path.empty()) {
        std::cerr << "--config is required for '" << command << "'\n";
        return 2;
    }

    auto cfg = altmacro::Config::load(config_path);
    if (!cfg) {
        std::cerr << "configuration: " << cfg.error().message << "\n";
        return 2;
    }
    altmacro::log::init("altmacro", cfg->instance_id, cfg->log_level);

    if (command == "run") return altmacro::cli::run_service(*cfg);
    if (command == "bootstrap") return altmacro::cli::run_bootstrap(*cfg, gap_scan);
    if (command == "inspect") return altmacro::cli::run_inspect(*cfg, what);
    if (command == "nats-setup") return altmacro::cli::run_nats_setup(*cfg);

    std::cerr << "unknown command: " << command << "\n";
    usage();
    return 2;
}
