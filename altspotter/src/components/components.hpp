#pragma once

#include "config/config.hpp"

// The market-data components of the `altspotter run` service. Each is a
// blocking function that runs its loop until shutdown is requested and then
// returns. A non-shutdown return (failed pre-flight, e.g. PostgreSQL/NATS
// briefly unavailable) is treated by the runner as "restart this component" —
// systemd-style resilience at component granularity inside one process.
//
// Each receives its own slice of the unified configuration (`ctx.service`) plus
// the shared common block (`ctx.common`). Shutdown is coordinated through the
// process-wide ShutdownToken singleton.

namespace altspotter::components {

int instrument_catalog(config::ServiceContext ctx);
int market_gateway(config::ServiceContext ctx);
int microstructure_collector(config::ServiceContext ctx);
int market_history(config::ServiceContext ctx);
int storage_writer(config::ServiceContext ctx);
int bar_aggregation(config::ServiceContext ctx);
int data_integrity(config::ServiceContext ctx);

// Four-venue expansion: per-venue market-data runtimes (WS + context +
// microstructure persistence), plus retention/aggregation machinery.
int binance_usdm_runtime(config::ServiceContext ctx);
int bybit_runtime(config::ServiceContext ctx);
int okx_runtime(config::ServiceContext ctx);
int bitget_context(config::ServiceContext ctx);
int microstructure_aggregation(config::ServiceContext ctx);
int retention_manager(config::ServiceContext ctx);

}  // namespace altspotter::components
