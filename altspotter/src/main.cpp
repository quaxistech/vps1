// altspotter — a single, self-contained binary for the multi-venue USDT-perpetual
// market-data platform. The `run` command is the service (all ingestion/storage/
// history/integrity components in one process); the remaining commands are the
// operational tools folded in as subcommands.

#include "components/runner.hpp"
#include "ops/ops.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void usage() {
    std::cout <<
        "altspotter — self-contained multi-venue futures market-data platform\n"
        "\n"
        "usage: altspotter <command> [options]\n"
        "\n"
        "commands:\n"
        "  run            run the market-data service (all components in one process)\n"
        "  migrate        apply PostgreSQL schema migrations, then ensure partitions\n"
        "  nats-init      provision JetStream streams/consumers (idempotent)\n"
        "  config-check   validate the unified service config\n"
        "  inspect        stored-data report (--exchanges/--universe/--storage/...)\n"
        "  retention-run  one-shot retention pass + storage-budget report\n"
        "  version        print version and build metadata\n"
        "\n"
        "run:  altspotter run --config <altspotter.yaml> [--common <common.yaml>]\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string_view cmd = argv[1];

    if (cmd == "run") {
        std::string config_path;
        std::string common_path = "/etc/altspotter/common.yaml";
        std::vector<std::string> only;
        for (int i = 2; i < argc; ++i) {
            const std::string_view a = argv[i];
            if (a == "--config" && i + 1 < argc) {
                config_path = argv[++i];
            } else if (a == "--common" && i + 1 < argc) {
                common_path = argv[++i];
            } else if (a == "--only" && i + 1 < argc) {
                // comma-separated component section names, e.g. --only storage_writer
                std::string list = argv[++i];
                size_t pos = 0;
                while (pos < list.size()) {
                    const size_t comma = list.find(',', pos);
                    const std::string tok =
                        list.substr(pos, comma == std::string::npos ? comma : comma - pos);
                    if (!tok.empty()) only.push_back(tok);
                    if (comma == std::string::npos) break;
                    pos = comma + 1;
                }
            } else if (a == "--help" || a == "-h") {
                std::cout << "usage: altspotter run --config <altspotter.yaml> "
                             "[--common <common.yaml>] [--only c1,c2,...]\n";
                return 0;
            } else {
                std::cerr << "altspotter run: unknown argument: " << a << "\n";
                return 2;
            }
        }
        if (config_path.empty()) {
            std::cerr << "altspotter run: --config is required\n";
            return 2;
        }
        return altspotter::run_service(config_path, common_path, only);
    }

    // Operational subcommands: pass the tail so each sees argv[0] == subcommand.
    if (cmd == "migrate") return altspotter::ops::cmd_migrate(argc - 1, argv + 1);
    if (cmd == "nats-init") return altspotter::ops::cmd_nats_init(argc - 1, argv + 1);
    if (cmd == "config-check") return altspotter::ops::cmd_config_check(argc - 1, argv + 1);
    if (cmd == "inspect") return altspotter::ops::cmd_inspect(argc - 1, argv + 1);
    if (cmd == "retention-run") return altspotter::ops::cmd_retention(argc - 1, argv + 1);
    if (cmd == "version" || cmd == "--version" || cmd == "-v") {
        return altspotter::ops::cmd_version(argc - 1, argv + 1);
    }
    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        usage();
        return 0;
    }

    std::cerr << "altspotter: unknown command '" << cmd << "'\n\n";
    usage();
    return 2;
}
