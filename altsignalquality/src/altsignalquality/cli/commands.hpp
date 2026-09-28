#pragma once

namespace altsignalquality::cli {

// Subcommand entry points. Each receives argv shifted so argv[0] is the
// subcommand name, and each handles --help itself (usage on stdout, return 0).
//
// Exit-code convention: 0 ok / clean stop, 1 runtime failure or a failed audit,
// 2 usage/config error; `train` adds 3 = deferred under resource pressure.
int cmd_run(int argc, char** argv);
int cmd_train(int argc, char** argv);
int cmd_validate(int argc, char** argv);
int cmd_model_status(int argc, char** argv);
int cmd_dataset_build(int argc, char** argv);
int cmd_nats_setup(int argc, char** argv);
int cmd_ablation(int argc, char** argv);

} // namespace altsignalquality::cli
