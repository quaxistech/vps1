// The reference AltSignalGateway protocol client.
//
// This is not a probe. It performs the whole lifecycle an external consumer has
// to implement — TLS, HELLO, AUTH, RESUME, SIGNAL, ACK, HEARTBEAT, GOODBYE —
// against the real server, and it is what the integration tests drive. Its
// existence is also the answer to "how does VPS2 connect": read this file.
//
// It shares the canonical auth string and the frame codecs with the server, so
// the two cannot drift apart.

#include "altsignalgateway/cli/commands.hpp"

#include "altsignalgateway/core/codec.hpp"
#include "altsignalgateway/core/hash.hpp"
#include "altsignalgateway/core/shutdown.hpp"
#include "altsignalgateway/core/time.hpp"
#include "altsignalgateway/domain/auth.hpp"
#include "altsignalgateway/domain/protocol.hpp"
#include "gateway.pb.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>

#include <google/protobuf/util/json_util.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <functional>
#include <cstdlib>
#include <fstream>
#include <string>

namespace altsignalgateway::cli {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace asio = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;
namespace pb = altsignalgateway::v1;

namespace {

void usage() {
    std::fputs(
        "Usage: altsignalgateway client [options]\n"
        "\n"
        "  --host HOST              gateway host (default 127.0.0.1)\n"
        "  --port PORT              gateway port (default 9443)\n"
        "  --sni NAME               TLS server name (default: --host)\n"
        "  --ca-file PATH           CA bundle used to verify the server certificate\n"
        "  --client-id ID           the client identity\n"
        "  --key-id ID              which key of that identity to sign with\n"
        "  --secret-file PATH       file holding the hex secret (preferred)\n"
        "  --secret-env VAR         environment variable holding the hex secret\n"
        "  --cert PATH --key PATH   client certificate, when the server requires mTLS\n"
        "  --resume MODE            from-sequence | live | from-start  (default from-sequence)\n"
        "  --last-acked N           position for --resume from-sequence (default 0)\n"
        "  --count N                exit after N signals (0 = run until interrupted)\n"
        "  --no-ack                 receive without acknowledging (slow-consumer test)\n"
        "  --ack-nth N              acknowledge only every Nth message (batch acknowledgement,\n"
        "                           and the way an out-of-order cursor bug shows itself)\n"
        "  --idle-exit SEC          exit after SEC seconds with no signal (0 = never)\n"
        "  --json                   print each signal as JSON instead of a summary line\n"
        "  --quiet                  suppress per-signal output (the final summary still prints)\n"
        "\n"
        "Query mode (protocol 2). Sends ONE query after AUTH_OK, prints the response as\n"
        "JSON, verifies any carried payload against its hash, and exits (0 = answered,\n"
        "2 = usage, 3 = the server answered with a QueryError):\n"
        "  --query signal|history|context\n"
        "  --signal-id ID | --sequence N      (signal, context)\n"
        "  --detail standard|rich|full        (context; default rich)\n"
        "  --symbol S --pair N --exchange E --strategy S --side long|short\n"
        "  --state JOURNALED|QUEUED|SENT|ACKED|EXPIRED|FAILED\n"
        "  --from-ms T0 --to-ms T1 --page-size N --cursor TOKEN   (history)\n",
        stderr);
}

// One query, one answer. Heartbeats that arrive in between are answered so the
// session stays alive; anything else before the QueryResponse is a protocol
// surprise and is reported. Returns the process exit code.
int run_query(const std::function<void(const pb::ClientFrame&)>& send,
              const std::function<bool(pb::ServerFrame&)>& recv,
              const pb::QueryRequest& request, bool quiet) {
    {
        pb::ClientFrame f;
        *f.mutable_query() = request;
        send(f);
    }
    for (;;) {
        pb::ServerFrame frame;
        if (!recv(frame)) {
            std::fprintf(stderr, "malformed server frame\n");
            return 1;
        }
        if (frame.has_heartbeat()) {
            pb::ClientFrame f;
            f.mutable_heartbeat_ack()->set_nonce(frame.heartbeat().nonce());
            f.mutable_heartbeat_ack()->set_client_time_ms(timeutil::now_ms());
            send(f);
            continue;
        }
        if (frame.has_error()) {
            std::fprintf(stderr, "server error: %s (%s)\n", frame.error().message().c_str(),
                         pb::ErrorCode_Name(frame.error().code()).c_str());
            return 1;
        }
        if (!frame.has_query_response()) {
            std::fprintf(stderr, "unexpected frame %d while waiting for the query response\n",
                         static_cast<int>(frame.body_case()));
            return 1;
        }
        const auto& resp = frame.query_response();
        if (resp.request_id() != request.request_id()) {
            std::fprintf(stderr, "response for another request (%s)\n", resp.request_id().c_str());
            return 1;
        }
        // Integrity: a queried payload re-hashes to the envelope's value, the
        // same check a delivered frame gets.
        int verified = 0;
        int mismatched = 0;
        const auto check = [&](const pb::SignalMessage& m) {
            const auto h = messaging::canonical_hash(m.payload());
            if (h && *h == m.envelope().payload_sha256()) ++verified;
            else ++mismatched;
        };
        if (resp.has_signal()) check(resp.signal().signal());
        if (resp.has_context()) check(resp.context().signal());
        std::string json;
        google::protobuf::util::JsonPrintOptions opts;
        opts.add_whitespace = true;
        opts.always_print_primitive_fields = true;
        (void)google::protobuf::util::MessageToJsonString(resp, &json, opts);
        std::printf("%s\n", json.c_str());
        if (!quiet) {
            std::fprintf(stderr, "payload hashes verified=%d mismatched=%d\n", verified, mismatched);
        }
        if (mismatched > 0) return 1;
        return resp.has_error() ? 3 : 0;
    }
}

std::string read_secret(const Args& args) {
    const std::string file = args.option("--secret-file");
    if (!file.empty()) {
        std::ifstream in(file);
        if (!in) {
            std::fprintf(stderr, "cannot read --secret-file %s\n", file.c_str());
            return {};
        }
        std::string hex;
        std::getline(in, hex);
        while (!hex.empty() && (hex.back() == '\n' || hex.back() == '\r' || hex.back() == ' ')) {
            hex.pop_back();
        }
        return hex;
    }
    const std::string var = args.option("--secret-env");
    if (!var.empty()) {
        const char* v = std::getenv(var.c_str());
        if (v == nullptr) {
            std::fprintf(stderr, "environment variable %s is not set\n", var.c_str());
            return {};
        }
        return v;
    }
    // Never a command-line option: an argv secret is visible in `ps` to every
    // user on the box.
    std::fprintf(stderr, "one of --secret-file or --secret-env is required\n");
    return {};
}

std::string signal_summary(const pb::SignalMessage& msg) {
    const auto& e = msg.envelope();
    const auto& t = msg.payload().trade_plan();
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "seq=%lld %s %s %s entry=%.10g stop=%.10g target=%.10g expires=%s%s "
                  "sha256=%.16s",
                  static_cast<long long>(e.sequence()), e.signal_id().c_str(),
                  t.symbol().empty() ? "<unresolved>" : t.symbol().c_str(),
                  altstrategy::v1::Side_Name(t.side()).c_str(), t.entry_price(),
                  t.stop_loss_price(), t.take_profit_price(),
                  timeutil::to_iso8601(e.expires_at_ms()).c_str(),
                  e.replay() ? " [replay]" : "", e.payload_sha256().c_str());
    return buf;
}

} // namespace

int cmd_client(const Args& args) {
    if (args.flag("--help")) {
        usage();
        return 0;
    }
    const std::string host = args.option("--host", "127.0.0.1");
    const std::string port = args.option("--port", "9443");
    const std::string sni = args.option("--sni", host);
    const std::string ca_file = args.option("--ca-file");
    const std::string client_id = args.option("--client-id");
    const std::string key_id = args.option("--key-id");
    const std::string resume_mode = args.option("--resume", "from-sequence");
    bool args_ok = true;
    bool one_ok = true;
    const int64_t last_acked = args.int_option("--last-acked", 0, &one_ok); args_ok &= one_ok;
    const int64_t want = args.int_option("--count", 0, &one_ok); args_ok &= one_ok;
    const int64_t idle_exit_sec = args.int_option("--idle-exit", 0, &one_ok); args_ok &= one_ok;
    const bool no_ack = args.flag("--no-ack");
    // Acknowledging in batches is legitimate consumer behaviour: a consumer that
    // commits every N trades acknowledges the Nth and nothing between. It is
    // also exactly the shape that exposes a server whose durable cursor follows
    // the highest ACK rather than the contiguous one.
    const int64_t ack_nth = args.int_option("--ack-nth", 1, &one_ok); args_ok &= one_ok;
    if (!args_ok) return 2;
    if (last_acked < 0 || want < 0 || idle_exit_sec < 0 || ack_nth < 1) {
        std::fprintf(stderr, "--last-acked/--count/--idle-exit must be >= 0 and --ack-nth >= 1\n");
        return 2;
    }
    const bool as_json = args.flag("--json");
    const bool quiet = args.flag("--quiet");

    // ---- query mode -----------------------------------------------------
    const std::string query_kind = args.option("--query");
    pb::QueryRequest query;
    if (!query_kind.empty()) {
        query.set_request_id("cli-" + std::to_string(timeutil::now_ms()));
        const std::string signal_id = args.option("--signal-id");
        const int64_t sequence = args.int_option("--sequence", 0, &one_ok); args_ok &= one_ok;
        if (query_kind == "signal") {
            auto* g = query.mutable_get_signal();
            if (!signal_id.empty()) g->set_signal_id(signal_id);
            if (sequence > 0) g->set_sequence(sequence);
        } else if (query_kind == "context") {
            auto* c = query.mutable_get_context();
            c->set_signal_id(signal_id);
            const std::string detail = args.option("--detail", "rich");
            c->set_detail(detail == "standard" ? pb::CONTEXT_STANDARD
                          : detail == "full"  ? pb::CONTEXT_FULL
                                              : pb::CONTEXT_RICH);
        } else if (query_kind == "history") {
            auto* h = query.mutable_get_history();
            auto* f = h->mutable_filter();
            f->set_symbol(args.option("--symbol"));
            f->set_canonical_pair_id(args.int_option("--pair", 0, &one_ok)); args_ok &= one_ok;
            f->set_exchange(args.option("--exchange"));
            f->set_strategy_id(args.option("--strategy"));
            const std::string side = args.option("--side");
            if (side == "long") f->set_side(altstrategy::v1::SIDE_LONG);
            if (side == "short") f->set_side(altstrategy::v1::SIDE_SHORT);
            f->set_delivery_state(args.option("--state"));
            f->set_from_created_at_ms(args.int_option("--from-ms", 0, &one_ok)); args_ok &= one_ok;
            f->set_to_created_at_ms(args.int_option("--to-ms", 0, &one_ok)); args_ok &= one_ok;
            h->set_page_size(static_cast<int32_t>(args.int_option("--page-size", 0, &one_ok)));
            args_ok &= one_ok;
            h->set_cursor(args.option("--cursor"));
        } else {
            std::fprintf(stderr, "--query must be signal, history or context\n");
            return 2;
        }
        if (!args_ok) return 2;
    }

    if (client_id.empty() || key_id.empty()) {
        std::fprintf(stderr, "--client-id and --key-id are required\n");
        return 2;
    }
    const std::string secret_hex = read_secret(args);
    if (secret_hex.empty()) return 2;
    auto secret = domain::hex_decode(secret_hex);
    if (!secret) {
        std::fprintf(stderr, "secret is not valid hex\n");
        return 2;
    }

    try {
        asio::io_context io;
        ssl::context ctx(ssl::context::tls_client);
        ctx.set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 |
                        ssl::context::no_sslv3 | ssl::context::no_tlsv1 |
                        ssl::context::no_tlsv1_1);
        if (!ca_file.empty()) {
            ctx.load_verify_file(ca_file);
        } else {
            ctx.set_default_verify_paths();
        }
        // There is no --insecure. A client that skips verification is a client
        // that can be handed someone else's trades.
        ctx.set_verify_mode(ssl::verify_peer);
        ctx.set_verify_callback(ssl::host_name_verification(sni));
        const std::string cert = args.option("--cert");
        const std::string key = args.option("--key");
        if (!cert.empty() && !key.empty()) {
            ctx.use_certificate_chain_file(cert);
            ctx.use_private_key_file(key, ssl::context::pem);
        }

        tcp::resolver resolver(io);
        websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws(io, ctx);
        const auto endpoints = resolver.resolve(host, port);
        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(15));
        beast::get_lowest_layer(ws).connect(endpoints);
        if (SSL_set_tlsext_host_name(ws.next_layer().native_handle(), sni.c_str()) == 0) {
            std::fprintf(stderr, "failed to set TLS SNI\n");
            return 1;
        }
        ws.next_layer().handshake(ssl::stream_base::client);
        beast::get_lowest_layer(ws).expires_never();
        ws.binary(true);
        ws.read_message_max(16 * 1024 * 1024);
        ws.handshake(host + ":" + port, "/");

        const auto send = [&](const pb::ClientFrame& frame) {
            std::string bytes;
            frame.SerializeToString(&bytes);
            ws.write(asio::buffer(bytes));
        };
        // A clean WebSocket close after GOODBYE. Without it the TLS stream is
        // truncated and the server records an orderly departure as a read
        // failure, which turns every normal client exit into a log line an
        // operator has to learn to ignore.
        const auto depart = [&](const char* reason) {
            try {
                pb::ClientFrame f;
                f.mutable_goodbye()->set_reason(reason);
                send(f);
                beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(5));
                ws.close(websocket::close_code::normal);
            } catch (const std::exception&) {
                // The connection is going away either way.
            }
        };
        const auto recv = [&](pb::ServerFrame& out) {
            beast::flat_buffer buffer;
            ws.read(buffer);
            const auto data = buffer.cdata();
            return out.ParseFromArray(data.data(), static_cast<int>(data.size()));
        };

        // ---- HELLO ----------------------------------------------------------
        const std::string client_nonce = domain::random_bytes(16);
        {
            pb::ClientFrame f;
            auto* hello = f.mutable_hello();
            hello->set_protocol_version(domain::kProtocolVersion);
            hello->set_client_id(client_id);
            hello->set_client_version(std::string("altsignalgateway-client/") +
                                      ALTSIGNALGATEWAY_VERSION);
            hello->add_supported_schema_versions(domain::kSchemaVersion);
            hello->set_client_nonce(client_nonce);
            send(f);
        }
        pb::ServerFrame reply;
        if (!recv(reply) || !reply.has_hello_ok()) {
            if (reply.has_error()) {
                std::fprintf(stderr, "server refused HELLO: %s (%s)\n",
                             reply.error().message().c_str(),
                             pb::ErrorCode_Name(reply.error().code()).c_str());
            } else {
                std::fprintf(stderr, "expected HELLO_OK\n");
            }
            return 1;
        }
        const auto hello_ok = reply.hello_ok();

        // ---- AUTH -----------------------------------------------------------
        {
            const int64_t ts = timeutil::now_ms();
            const std::string canonical = domain::auth_canonical_string(
                domain::kProtocolVersion, client_id, key_id, hello_ok.session_id(),
                hello_ok.server_nonce(), client_nonce, ts);
            pb::ClientFrame f;
            auto* auth = f.mutable_auth();
            auth->set_key_id(key_id);
            auth->set_timestamp_ms(ts);
            auth->set_server_nonce(hello_ok.server_nonce());
            auth->set_signature(domain::hmac_sha256(*secret, canonical));
            send(f);
        }
        if (!recv(reply) || !reply.has_auth_ok()) {
            if (reply.has_error()) {
                std::fprintf(stderr, "authentication failed: %s (%s)\n",
                             reply.error().message().c_str(),
                             pb::ErrorCode_Name(reply.error().code()).c_str());
            } else {
                std::fprintf(stderr, "expected AUTH_OK\n");
            }
            return 1;
        }
        const auto auth_ok = reply.auth_ok();
        if (!quiet) {
            std::fprintf(stderr,
                         "authenticated session=%s server_cursor=%lld journal_head=%lld\n",
                         auth_ok.session_id().c_str(),
                         static_cast<long long>(auth_ok.last_acked_sequence()),
                         static_cast<long long>(auth_ok.journal_head_sequence()));
        }

        if (!query_kind.empty()) {
            const int rc = run_query(send, recv, query, quiet);
            pb::ClientFrame bye;
            bye.mutable_goodbye()->set_reason("query done");
            try { send(bye); } catch (const std::exception&) {}
            return rc;
        }

        // ---- RESUME ---------------------------------------------------------
        {
            pb::ClientFrame f;
            auto* r = f.mutable_resume();
            if (resume_mode == "live") {
                r->set_mode(pb::RESUME_LIVE_ONLY);
            } else if (resume_mode == "from-start") {
                r->set_mode(pb::RESUME_FROM_START);
            } else {
                r->set_mode(pb::RESUME_FROM_SEQUENCE);
                r->set_last_acked_sequence(last_acked);
            }
            send(f);
        }

        // ---- stream ---------------------------------------------------------
        int64_t received = 0;
        int64_t acked = 0;
        int64_t last_activity = timeutil::now_ms();
        auto& shutdown = ShutdownToken::instance();

        while (!shutdown.stop_requested()) {
            if (idle_exit_sec > 0 &&
                timeutil::now_ms() - last_activity > idle_exit_sec * 1000) {
                if (!quiet) std::fprintf(stderr, "idle timeout reached\n");
                break;
            }
            beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(
                idle_exit_sec > 0 ? idle_exit_sec + 1 : 120));
            pb::ServerFrame frame;
            try {
                if (!recv(frame)) {
                    std::fprintf(stderr, "malformed server frame\n");
                    return 1;
                }
            } catch (const beast::system_error& e) {
                if (e.code() == websocket::error::closed) break;
                if (e.code() == beast::error::timeout && idle_exit_sec > 0) break;
                throw;
            }
            beast::get_lowest_layer(ws).expires_never();

            switch (frame.body_case()) {
                case pb::ServerFrame::kResumeOk:
                    if (!quiet) {
                        std::fprintf(stderr,
                                     "resume from=%lld backlog=%lld expired_in_range=%lld "
                                     "head=%lld\n",
                                     static_cast<long long>(frame.resume_ok().from_sequence_exclusive()),
                                     static_cast<long long>(frame.resume_ok().backlog_count()),
                                     static_cast<long long>(frame.resume_ok().skipped_expired()),
                                     static_cast<long long>(frame.resume_ok().journal_head_sequence()));
                    }
                    break;
                case pb::ServerFrame::kResumeComplete:
                    if (!quiet) {
                        std::fprintf(stderr, "replay complete: %lld message(s) through seq %lld\n",
                                     static_cast<long long>(frame.resume_complete().delivered()),
                                     static_cast<long long>(frame.resume_complete().last_sequence()));
                    }
                    break;
                case pb::ServerFrame::kReady:
                    if (!quiet) std::fprintf(stderr, "ready\n");
                    break;
                case pb::ServerFrame::kHeartbeat: {
                    pb::ClientFrame f;
                    auto* hb = f.mutable_heartbeat_ack();
                    hb->set_nonce(frame.heartbeat().nonce());
                    hb->set_client_time_ms(timeutil::now_ms());
                    send(f);
                    break;
                }
                case pb::ServerFrame::kSignal: {
                    ++received;
                    last_activity = timeutil::now_ms();
                    const auto& msg = frame.signal();
                    // Verify the integrity hash before doing anything else with
                    // the trade. A consumer that acts first and checks later is
                    // a consumer that acts on corrupted instructions.
                    auto canonical = messaging::canonical_serialize(msg.payload());
                    const bool hash_ok =
                        canonical && sha256_hex(*canonical) == msg.envelope().payload_sha256();
                    if (!hash_ok) {
                        std::fprintf(stderr,
                                     "PAYLOAD HASH MISMATCH on seq=%lld signal_id=%s — not acked\n",
                                     static_cast<long long>(msg.envelope().sequence()),
                                     msg.envelope().signal_id().c_str());
                        return 1;
                    }
                    if (!quiet) {
                        if (as_json) {
                            std::string json;
                            google::protobuf::util::JsonPrintOptions opts;
                            opts.add_whitespace = true;
                            opts.always_print_primitive_fields = true;
                            opts.preserve_proto_field_names = true;
                            (void)google::protobuf::util::MessageToJsonString(msg, &json, opts);
                            std::printf("%s\n", json.c_str());
                        } else {
                            std::printf("%s\n", signal_summary(msg).c_str());
                        }
                        std::fflush(stdout);
                    }
                    if (!no_ack && (ack_nth <= 1 || (received % ack_nth) == 0)) {
                        pb::ClientFrame f;
                        auto* ack = f.mutable_ack();
                        ack->set_message_id(msg.envelope().message_id());
                        ack->set_signal_id(msg.envelope().signal_id());
                        ack->set_sequence(msg.envelope().sequence());
                        ack->set_client_time_ms(timeutil::now_ms());
                        send(f);
                        ++acked;
                    }
                    if (want > 0 && received >= want) {
                        depart("requested count reached");
                        // Always printed, even under --quiet: this is the
                        // outcome of the run, and a harness that suppresses it
                        // has no way to tell success from silence.
                        std::fprintf(stderr, "received=%lld acked=%lld\n",
                                     static_cast<long long>(received),
                                     static_cast<long long>(acked));
                        return 0;
                    }
                    break;
                }
                case pb::ServerFrame::kError:
                    std::fprintf(stderr, "server error: %s (%s)%s\n",
                                 frame.error().message().c_str(),
                                 pb::ErrorCode_Name(frame.error().code()).c_str(),
                                 frame.error().fatal() ? " [fatal]" : "");
                    if (frame.error().fatal()) return 1;
                    break;
                case pb::ServerFrame::kGoodbye:
                    if (!quiet) {
                        std::fprintf(stderr, "server goodbye: %s\n",
                                     frame.goodbye().reason().c_str());
                    }
                    return 0;
                default:
                    break;
            }
        }
        depart("client finished");
        std::fprintf(stderr, "received=%lld acked=%lld\n", static_cast<long long>(received),
                     static_cast<long long>(acked));
        return (want > 0 && received < want) ? 1 : 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "client error: %s\n", e.what());
        return 1;
    }
}

} // namespace altsignalgateway::cli
