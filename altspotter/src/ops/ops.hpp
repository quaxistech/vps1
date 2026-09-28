#pragma once

// Operational subcommands of the single `altspotter` binary. Each mirrors a
// former standalone tool and keeps its own argument parsing; argv is the tail
// after the subcommand name (argv[0] == subcommand).

namespace altspotter::ops {

int cmd_migrate(int argc, char** argv);       // schema migrations + partition ensure
int cmd_nats_init(int argc, char** argv);     // idempotent JetStream provisioning
int cmd_config_check(int argc, char** argv);  // validate the unified config
int cmd_inspect(int argc, char** argv);       // stored-history operational report
int cmd_retention(int argc, char** argv);     // one-shot retention pass + budget report
int cmd_version(int argc, char** argv);       // version / build metadata

}  // namespace altspotter::ops
