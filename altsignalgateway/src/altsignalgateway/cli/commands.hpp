#pragma once

#include <string>
#include <vector>

namespace altsignalgateway::cli {

struct Args {
    std::string config;
    std::vector<std::string> rest;

    [[nodiscard]] std::string option(const std::string& name, const std::string& def = {}) const;
    [[nodiscard]] bool flag(const std::string& name) const;
    // Parse an integer option. std::stoll throws on anything it does not like,
    // and an uncaught throw out of argument parsing is a std::terminate — a
    // crash on a typo. `ok` is set to false instead; callers report and exit.
    [[nodiscard]] int64_t int_option(const std::string& name, int64_t def, bool* ok) const;
};

int cmd_run(const Args& args);
int cmd_nats_setup(const Args& args);
int cmd_validate(const Args& args);
int cmd_status(const Args& args);
int cmd_inspect(const Args& args);
int cmd_verify(const Args& args);
// The database invariants, exercised against the real database inside a
// transaction that is rolled back. Safe to run on a live deployment.
int cmd_selftest(const Args& args);
// The reference protocol client. It is a real client, not a probe: it performs
// the full handshake, resumes, acknowledges and answers heartbeats, and it is
// the executable definition of what an external consumer has to implement.
int cmd_client(const Args& args);

} // namespace altsignalgateway::cli
