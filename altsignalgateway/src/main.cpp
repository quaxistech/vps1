// AltSignalGateway — VPS1's external signal boundary.
//
//   AltSpotter -> AltTech -> AltML -> AltStrategy -> AltSignalQuality
//                                                          |
//                                                    FINAL TradeSignal
//                                                          |
//                                                   AltSignalGateway
//                                                          |
//                                            durable journal + TLS WebSocket
//
// One binary, one process, one systemd unit. It takes no trading decision: it
// validates, canonicalizes, journals, numbers, hashes and delivers, and it
// never modifies the trade it carries.

#include "altsignalgateway/cli/commands.hpp"
#include "altsignalgateway/core/logger.hpp"
#include "altsignalgateway/core/shutdown.hpp"

#include <google/protobuf/stubs/common.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

void print_usage() {
    std::fputs(
        "altsignalgateway " ALTSIGNALGATEWAY_VERSION " — VPS1 FINAL TradeSignal boundary\n"
        "\n"
        "Usage: altsignalgateway <command> [--config PATH] [options]\n"
        "\n"
        "Service:\n"
        "  run             consume FINAL signals, journal them and serve the TLS WebSocket\n"
        "  nats-setup      validate the input stream and create this service's durable\n"
        "\n"
        "Operations (read-only unless stated):\n"
        "  validate        check the configuration, credentials, TLS material and database\n"
        "  status          print journal, delivery and client state as JSON\n"
        "  inspect         dump one journalled signal (--signal-id or --sequence)\n"
        "  verify          re-hash journalled payloads and report any mismatch\n"
        "  selftest        prove the database invariants on this deployment (rolled back)\n"
        "\n"
        "Client:\n"
        "  client          the reference protocol client (connect, resume, receive, ack)\n"
        "\n"
        "Common options:\n"
        "  --config PATH   configuration file (default /etc/altsignalgateway/altsignalgateway.yaml)\n"
        "  --version       print the build version\n"
        "  --help          this text\n",
        stderr);
}

} // namespace

int main(int argc, char** argv) {
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    if (argc < 2) {
        print_usage();
        return 2;
    }
    const std::string command = argv[1];
    if (command == "--help" || command == "-h" || command == "help") {
        print_usage();
        return 0;
    }
    if (command == "--version" || command == "-V") {
        std::printf("%s\n", ALTSIGNALGATEWAY_VERSION);
        return 0;
    }

    altsignalgateway::cli::Args args;
    args.config = "/etc/altsignalgateway/altsignalgateway.yaml";
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) {
            args.config = argv[++i];
            continue;
        }
        args.rest.push_back(a);
    }

    altsignalgateway::ShutdownToken::instance().install_signal_handlers();

    int rc = 2;
    if (command == "run") {
        rc = altsignalgateway::cli::cmd_run(args);
    } else if (command == "nats-setup") {
        rc = altsignalgateway::cli::cmd_nats_setup(args);
    } else if (command == "validate") {
        rc = altsignalgateway::cli::cmd_validate(args);
    } else if (command == "status") {
        rc = altsignalgateway::cli::cmd_status(args);
    } else if (command == "inspect") {
        rc = altsignalgateway::cli::cmd_inspect(args);
    } else if (command == "verify") {
        rc = altsignalgateway::cli::cmd_verify(args);
    } else if (command == "selftest") {
        rc = altsignalgateway::cli::cmd_selftest(args);
    } else if (command == "client") {
        rc = altsignalgateway::cli::cmd_client(args);
    } else {
        std::fprintf(stderr, "unknown command: %s\n\n", command.c_str());
        print_usage();
        rc = 2;
    }

    google::protobuf::ShutdownProtobufLibrary();
    return rc;
}
