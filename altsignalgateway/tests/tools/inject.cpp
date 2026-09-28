// asgw-inject — an adversarial publisher for the FINAL signal subject.
//
// WHY THIS IS A SEPARATE BINARY. AltSignalGateway's own NATS account has no
// publish permission on `signalquality.>` and must never have one: it is a
// consumer at a security boundary. So the only honest way to test what it does
// with a malformed, truncated, oversized or contradictory inbound message is to
// publish one as the PRODUCER, from a tool that is not part of the service.
//
// It is built only when tests are enabled and is never installed by the release.
// It takes the producer's credentials from the environment, so running it
// requires deliberately holding them.
//
//   asgw-inject --url nats://user:pass@127.0.0.1:4222 --subject signalquality.final.probe.v1 \
//               --case garbage|truncated|empty|not-final|wrong-service|bad-schema|
//                      rejected-decision|bad-geometry|nan-price|no-expiry|oversized|
//                      duplicate|conflict --source-file <journalled source payload>
//
// `duplicate` and `conflict` re-publish a REAL signal's bytes: the first
// unchanged, the second with one price altered, which is precisely the
// "same signal id, different payload" case the journal must refuse to
// overwrite.

#include "vendor/signalquality.pb.h"

#include <nats.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace sq = altsignalquality::v1;
namespace as = altstrategy::v1;

namespace {

std::string option(int argc, char** argv, const std::string& name, const std::string& def = {}) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (name == argv[i]) return argv[i + 1];
    }
    return def;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// A structurally valid FINAL signal, so each adversarial case can break exactly
// one thing and nothing else.
sq::Envelope well_formed(int64_t now_ms, const std::string& suffix) {
    sq::Envelope env;
    env.set_schema_version(1);
    env.set_message_id("inject_" + suffix);
    env.set_event_type("final_trade_signal");
    env.set_canonical_pair_id(1);
    env.set_symbol("probe");
    env.set_produced_at_ms(now_ms);
    env.set_source_service("altsignalquality");

    auto* fin = env.mutable_final_trade_signal();
    fin->set_final_signal_id("sqfin_inject_" + suffix);
    fin->set_published_at_ms(now_ms);
    fin->set_producer_version("inject");

    auto* p = fin->mutable_proposal();
    p->set_signal_proposal_id("sig_inject_" + suffix);
    p->set_candidate_id("cand_inject");
    p->set_canonical_pair_id(1);
    p->set_strategy_id("inject");
    p->set_strategy_version("0");
    p->set_side(as::SIDE_LONG);
    p->set_proposal_grade(as::GRADE_NORMAL);
    p->set_entry_type(as::ENTRY_MARKET);
    p->set_entry_price(100.0);
    p->set_stop_loss_price(99.0);
    p->set_take_profit_price(101.0);
    p->set_created_at_ms(now_ms);
    p->set_valid_until_ms(now_ms + 600000);
    p->set_max_holding_time_ms(3600000);

    auto* q = fin->mutable_quality();
    q->set_evaluation_id("sqev_inject_" + suffix);
    q->set_decision(sq::DECISION_ACCEPTED);
    q->set_basis(sq::BASIS_COLD_START);
    q->set_signal_created_at_ms(now_ms);
    q->set_evaluated_at_ms(now_ms);
    return env;
}

} // namespace

int main(int argc, char** argv) {
    const std::string url = option(argc, argv, "--url");
    const std::string subject = option(argc, argv, "--subject");
    const std::string kase = option(argc, argv, "--case");
    const std::string source_file = option(argc, argv, "--source-file");
    if (url.empty() || subject.empty() || kase.empty()) {
        std::fprintf(stderr,
                     "usage: asgw-inject --url URL --subject SUBJECT --case CASE"
                     " [--source-file PATH]\n");
        return 2;
    }
    const int64_t now = static_cast<int64_t>(::time(nullptr)) * 1000;
    const std::string suffix = std::to_string(now);

    std::string payload;
    std::string msg_id;

    if (kase == "garbage") {
        // 512 bytes that are not protobuf at all.
        payload.assign(512, '\xAB');
        msg_id = "inject-garbage-" + suffix;
    } else if (kase == "truncated") {
        const auto env = well_formed(now, suffix);
        env.SerializeToString(&payload);
        payload.resize(payload.size() / 2);   // a valid message, cut in half
        msg_id = "inject-truncated-" + suffix;
    } else if (kase == "empty") {
        payload.clear();
        msg_id = "inject-empty-" + suffix;
    } else if (kase == "not-final") {
        // A valid envelope carrying an evaluation instead of a final signal.
        sq::Envelope env;
        env.set_schema_version(1);
        env.set_source_service("altsignalquality");
        env.mutable_quality_evaluation()->set_evaluation_id("sqev_inject");
        env.SerializeToString(&payload);
        msg_id = "inject-notfinal-" + suffix;
    } else if (kase == "wrong-service") {
        auto env = well_formed(now, suffix);
        env.set_source_service("not-altsignalquality");
        env.SerializeToString(&payload);
        msg_id = "inject-wrongsvc-" + suffix;
    } else if (kase == "bad-schema") {
        auto env = well_formed(now, suffix);
        env.set_schema_version(99);
        env.SerializeToString(&payload);
        msg_id = "inject-badschema-" + suffix;
    } else if (kase == "rejected-decision") {
        auto env = well_formed(now, suffix);
        env.mutable_final_trade_signal()->mutable_quality()->set_decision(sq::DECISION_REJECTED);
        env.SerializeToString(&payload);
        msg_id = "inject-rejected-" + suffix;
    } else if (kase == "bad-geometry") {
        auto env = well_formed(now, suffix);
        // A long whose stop sits above its entry.
        env.mutable_final_trade_signal()->mutable_proposal()->set_stop_loss_price(200.0);
        env.SerializeToString(&payload);
        msg_id = "inject-geometry-" + suffix;
    } else if (kase == "nan-price") {
        auto env = well_formed(now, suffix);
        env.mutable_final_trade_signal()->mutable_proposal()->set_entry_price(
            std::numeric_limits<double>::quiet_NaN());
        env.SerializeToString(&payload);
        msg_id = "inject-nan-" + suffix;
    } else if (kase == "no-expiry") {
        auto env = well_formed(now, suffix);
        env.mutable_final_trade_signal()->mutable_proposal()->set_valid_until_ms(0);
        env.SerializeToString(&payload);
        msg_id = "inject-noexpiry-" + suffix;
    } else if (kase == "oversized") {
        auto env = well_formed(now, suffix);
        // Just under the broker's 1 MB max_payload, to exercise the size path
        // without being refused by NATS itself.
        env.mutable_final_trade_signal()->mutable_proposal()->set_stop_basis(
            std::string(900 * 1024, 'x'));
        env.SerializeToString(&payload);
        msg_id = "inject-oversized-" + suffix;
    } else if (kase == "duplicate" || kase == "conflict") {
        if (source_file.empty()) {
            std::fprintf(stderr, "--case %s needs --source-file\n", kase.c_str());
            return 2;
        }
        payload = read_file(source_file);
        if (payload.empty()) {
            std::fprintf(stderr, "could not read %s\n", source_file.c_str());
            return 2;
        }
        if (kase == "conflict") {
            // Re-publish the SAME final_signal_id with a different trade. This
            // is the one inbound condition the journal must never resolve by
            // overwriting.
            sq::Envelope env;
            if (!env.ParseFromString(payload)) {
                std::fprintf(stderr, "%s is not an altsignalquality.v1 envelope\n",
                             source_file.c_str());
                return 2;
            }
            // The mutation must keep the signal VALID, or it is refused on
            // geometry before it ever reaches the idempotency comparison and
            // the integrity path is never exercised. Multiplying the target by
            // a constant does that for a long and breaks it for a short.
            // Moving the target 10% closer to the entry preserves the side
            // relationship either way, and still changes the trade materially.
            auto* p = env.mutable_final_trade_signal()->mutable_proposal();
            const double entry = p->entry_price();
            p->set_take_profit_price(entry + (p->take_profit_price() - entry) * 0.9);
            env.SerializeToString(&payload);
        }
        // A fresh dedup id: JetStream would otherwise swallow the republish and
        // the Gateway would never see it, which would test nothing.
        msg_id = "inject-" + kase + "-" + suffix;
    } else {
        std::fprintf(stderr, "unknown --case %s\n", kase.c_str());
        return 2;
    }

    natsConnection* conn = nullptr;
    natsOptions* opts = nullptr;
    natsStatus s = natsOptions_Create(&opts);
    if (s == NATS_OK) s = natsOptions_SetURL(opts, url.c_str());
    if (s == NATS_OK) s = natsOptions_SetTimeout(opts, 5000);
    if (s == NATS_OK) s = natsConnection_Connect(&conn, opts);
    if (s != NATS_OK) {
        std::fprintf(stderr, "connect failed: %s\n", natsStatus_GetText(s));
        return 1;
    }
    jsCtx* js = nullptr;
    s = natsConnection_JetStream(&js, conn, nullptr);
    if (s != NATS_OK) {
        std::fprintf(stderr, "jetstream failed: %s\n", natsStatus_GetText(s));
        return 1;
    }
    jsPubOptions po;
    jsPubOptions_Init(&po);
    po.MaxWait = 10000;
    po.MsgId = msg_id.c_str();
    jsPubAck* ack = nullptr;
    jsErrCode jerr = static_cast<jsErrCode>(0);
    s = js_Publish(&ack, js, subject.c_str(), payload.data(), static_cast<int>(payload.size()),
                   &po, &jerr);
    if (ack != nullptr) jsPubAck_Destroy(ack);
    if (s != NATS_OK) {
        std::fprintf(stderr, "publish failed: %s (js %d)\n", natsStatus_GetText(s),
                     static_cast<int>(jerr));
        return 1;
    }
    std::printf("published case=%s subject=%s bytes=%zu msg_id=%s\n", kase.c_str(),
                subject.c_str(), payload.size(), msg_id.c_str());
    jsCtx_Destroy(js);
    natsConnection_Destroy(conn);
    natsOptions_Destroy(opts);
    return 0;
}
