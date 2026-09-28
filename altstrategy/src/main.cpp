// altstrategy-service — the self-contained VPS1 adaptive decision engine, one
// binary / one process / one systemd unit.
//
// The composition root (app::Application) owns the shared PostgreSQL pool and
// NATS connection, holds a session advisory lock, and runs the event-driven
// decision engine + the internal maintenance scheduler: it assembles a versioned
// DecisionContext from AltTech/AltSpotter/AltML, runs the ten-strategy catalogue,
// ranks candidates adaptively, publishes concrete LONG/SHORT trade proposals,
// self-computes their market outcomes, and exports training snapshots for VPS2.
//
// It produces DECISIONS, not orders. It is self-contained: it does not require
// VPS2 for signals, outcomes, adaptation or snapshots.
//
//   altstrategy-service --config /etc/altstrategy/altstrategy.yaml
//                        --common /etc/altstrategy/common.yaml

#include "app/application.hpp"

#include "altstrategy/config/service_bootstrap.hpp"
#include "altstrategy/logging/logger.hpp"

#include <cstdio>
#include <utility>

using namespace altstrategy;

int main(int argc, char** argv) {
    auto ctx = config::bootstrap_service("altstrategy-service", argc, argv);
    if (!ctx) {
        std::fprintf(stderr, "bootstrap failed: %s\n", ctx.error().message.c_str());
        return 2;
    }

    app::Application application(std::move(*ctx));
    if (!application.setup()) return 1;
    return application.run();
}
