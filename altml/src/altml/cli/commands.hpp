#pragma once

namespace altml::cli {

// Subcommand entry points. Each receives argv shifted so that argv[0] is the
// subcommand name; each handles --help itself (usage on stdout, return 0).
// Exit-code convention: 0 ok / clean stop, 1 runtime failure, 2 usage/config
// error (train adds 3 = deferred under resource pressure).
int cmd_run(int argc, char** argv);
int cmd_train(int argc, char** argv);
int cmd_nats_setup(int argc, char** argv);
int cmd_model_validate(int argc, char** argv);
int cmd_replay(int argc, char** argv);

} // namespace altml::cli
