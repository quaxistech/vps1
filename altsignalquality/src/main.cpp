// altsignalquality: the last learned filter between AltStrategy's candidate
// trade proposals and VPS2.
//
//   altsignalquality run             the service: evaluator + dataset + trainer
//                                    + model-registry, one supervised process
//   altsignalquality train           one manual training cycle (diagnostics/CI)
//   altsignalquality validate        leakage / contract / model audit gate
//   altsignalquality model-status    registry, dataset and evaluation state
//   altsignalquality dataset-build   one manual dataset materialisation pass
//   altsignalquality nats-setup      manual JetStream provisioning
//
// It produces a DECISION about a signal, never an order: no position size,
// leverage, balance or exchange order id exists anywhere in this binary.

#include "altsignalquality/cli/commands.hpp"

#include <cstdio>
#include <string_view>

namespace {

void print_usage() {
    std::puts("usage: altsignalquality <command> [options]\n\n"
              "commands:\n"
              "  run             run the evaluator, dataset, trainer and model registry\n"
              "  train           run one bounded training cycle (diagnostics/CI)\n"
              "  validate        audit the contract, dataset, splits and ACTIVE model\n"
              "  model-status    print the registry, dataset and evaluation state\n"
              "  dataset-build   materialise priors and training examples once\n"
              "  nats-setup      provision/validate JetStream resources\n\n"
              "run 'altsignalquality <command> --help' for command options");
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
        std::puts(ALTSIGNALQUALITY_VERSION);
        return 0;
    }
    if (cmd == "run") return altsignalquality::cli::cmd_run(argc - 1, argv + 1);
    if (cmd == "train") return altsignalquality::cli::cmd_train(argc - 1, argv + 1);
    if (cmd == "validate") return altsignalquality::cli::cmd_validate(argc - 1, argv + 1);
    if (cmd == "model-status") return altsignalquality::cli::cmd_model_status(argc - 1, argv + 1);
    if (cmd == "dataset-build") return altsignalquality::cli::cmd_dataset_build(argc - 1, argv + 1);
    if (cmd == "nats-setup") return altsignalquality::cli::cmd_nats_setup(argc - 1, argv + 1);
    if (cmd == "ablation") return altsignalquality::cli::cmd_ablation(argc - 1, argv + 1);
    std::fprintf(stderr, "altsignalquality: unknown command '%s'\n", argv[1]);
    print_usage();
    return 2;
}
