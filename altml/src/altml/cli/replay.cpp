// altml replay: deterministic replay of a model's live predictions.
//
//   altml replay --model-id ID --from-ms MS --to-ms MS [--limit N] [--config F]
//
// Rebuilds each live prediction's input from the archive under the trainer's
// point-in-time rule, re-scores it twice with the registered artifact and
// calibration, and prints a JSON summary: how many live predictions were
// reproduced (to storage precision), how many had a different set of aligned
// sources available live than the archive shows, and whether any re-score
// differed from itself. Read-only. Exit: 0 ok, 1 failure, 2 usage.

#include "altml/cli/commands.hpp"

#include "altml/components/trainer.hpp"
#include "altml/core/config.hpp"
#include "altml/core/logger.hpp"

#include <cstdio>
#include <string>

namespace altml::cli {

int cmd_replay(int argc, char** argv) {
    std::string config_path = "/etc/altml/altml.yaml", model_id;
    long long from_ms = 0, to_ms = 0, limit = 100000;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) config_path = argv[++i];
        else if (a == "--model-id" && i + 1 < argc) model_id = argv[++i];
        else if (a == "--from-ms" && i + 1 < argc) from_ms = std::stoll(argv[++i]);
        else if (a == "--to-ms" && i + 1 < argc) to_ms = std::stoll(argv[++i]);
        else if (a == "--limit" && i + 1 < argc) limit = std::stoll(argv[++i]);
        else if (a == "--help" || a == "-h") {
            std::puts("usage: altml replay --model-id ID --from-ms MS --to-ms MS [--limit N] [--config F]");
            return 0;
        } else {
            std::fprintf(stderr, "altml replay: unknown argument '%s'\n", a.c_str());
            return 2;
        }
    }
    if (model_id.empty() || to_ms <= from_ms || limit <= 0) {
        std::fprintf(stderr, "altml replay: --model-id, --from-ms < --to-ms and --limit > 0 required\n");
        return 2;
    }
    auto rt = config::load_runtime(config_path);
    if (!rt) {
        std::fprintf(stderr, "config error: %s\n", rt.error().message.c_str());
        return 2;
    }
    log::init("altml", rt->common.instance_id, rt->common.log_level);
    return run_prediction_replay(*rt, model_id, from_ms, to_ms, limit);
}

} // namespace altml::cli
