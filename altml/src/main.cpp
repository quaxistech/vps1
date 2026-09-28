// altml: the unified AltML service binary.
//
//   altml run             long-running service (label-builder + trainer +
//                         model-registry + inference as supervised components)
//   altml train           one manual native training cycle (diagnostics/CI)
//   altml nats-setup      manual JetStream provisioning
//   altml model-validate  LightGBM artifact validation (audit gate)

#include "altml/cli/commands.hpp"

#include <cstdio>
#include <string_view>

namespace {

void print_usage() {
    std::puts("usage: altml <command> [options]\n\n"
              "commands:\n"
              "  run             run label-builder, trainer, model-registry and inference\n"
              "  train           run one bounded native training cycle (diagnostics/CI)\n"
              "  nats-setup      provision/validate JetStream resources\n"
              "  model-validate  validate a LightGBM artifact against recorded predictions\n"
              "  replay          re-score live econ predictions from the archive (parity check)\n\n"
              "run 'altml <command> --help' for command options");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 2;
    }
    const std::string_view cmd = argv[1];
    if (cmd == "--help" || cmd == "-h" || cmd == "help") {
        print_usage();
        return 0;
    }
    if (cmd == "--version") {
        std::puts(ALTML_VERSION);
        return 0;
    }
    if (cmd == "run") return altml::cli::cmd_run(argc - 1, argv + 1);
    if (cmd == "train") return altml::cli::cmd_train(argc - 1, argv + 1);
    if (cmd == "replay") return altml::cli::cmd_replay(argc - 1, argv + 1);
    if (cmd == "nats-setup") return altml::cli::cmd_nats_setup(argc - 1, argv + 1);
    if (cmd == "model-validate") return altml::cli::cmd_model_validate(argc - 1, argv + 1);
    std::fprintf(stderr, "altml: unknown command '%s'\n", argv[1]);
    print_usage();
    return 2;
}
