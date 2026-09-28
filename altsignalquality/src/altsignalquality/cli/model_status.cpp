// altsignalquality model-status: what the registry currently believes.
//
// Prints the running contract identity, the recent models with their metrics
// and lifecycle timestamps, the dataset size, and the live evaluation counters
// — everything needed to answer "which model is production, how good is it, and
// is the filter actually running" without opening psql.

#include "altsignalquality/cli/commands.hpp"

#include "altsignalquality/core/config.hpp"
#include "altsignalquality/core/logger.hpp"
#include "altsignalquality/core/pg.hpp"
#include "altsignalquality/data/store.hpp"

#include <cstdio>
#include <string>

namespace altsignalquality::cli {

namespace {

void print_usage() {
    std::puts("usage: altsignalquality model-status [--config PATH] [--limit N]\n\n"
              "Prints the model registry, dataset and evaluation state as JSON.");
}

} // namespace

int cmd_model_status(int argc, char** argv) {
    std::string config_path = "/etc/altsignalquality/altsignalquality.yaml";
    int limit = 10;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (a == "--limit" && i + 1 < argc) {
            limit = std::atoi(argv[++i]);
            if (limit < 1 || limit > 200) {
                std::fprintf(stderr, "--limit must be within [1, 200]\n");
                return 2;
            }
        } else if (a == "--help" || a == "-h") {
            print_usage();
            return 0;
        } else {
            std::fprintf(stderr, "altsignalquality model-status: unknown argument '%s'\n",
                         a.c_str());
            print_usage();
            return 2;
        }
    }
    auto rt = config::load_runtime(config_path);
    if (!rt) {
        std::fprintf(stderr, "config error: %s\n", rt.error().message.c_str());
        return 2;
    }
    log::init("altsignalquality", rt->common.instance_id, "warn");
    persistence::PgPool pool(rt->common.postgres_dsn, 1);
    data::Store store(pool, rt->wiring.altstrategy_schema, rt->wiring.max_stop_distance_bps);
    auto status = store.model_status_json(rt->wiring.objective, limit);
    if (!status) {
        std::fprintf(stderr, "registry read failed: %s\n", status.error().message.c_str());
        return 1;
    }
    std::printf("%s\n", status->c_str());
    return 0;
}

} // namespace altsignalquality::cli
