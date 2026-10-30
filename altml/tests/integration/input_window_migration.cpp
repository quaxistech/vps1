#include "altml/core/config.hpp"
#include "altml/core/logger.hpp"
#include "altml/core/nats_client.hpp"
#include "altml/core/nats_provision.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
struct Cursor { uint64_t ack = 0, delivered = 0; int64_t pending = 0; };
Cursor cursor(jsCtx* js) {
    jsConsumerInfo* info = nullptr; jsErrCode code;
    require(js_GetConsumerInfo(&info, js, "WINDOW_INPUT", "window-input", nullptr, &code) == NATS_OK,
            "fixture cursor unavailable");
    const Cursor result{info->AckFloor.Stream, info->Delivered.Stream, info->NumAckPending};
    jsConsumerInfo_Destroy(info);
    return result;
}
void settled(jsCtx* js) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        const auto value = cursor(js);
        if (value.pending == 0 && value.ack == value.delivered) return;
        nats_Sleep(20);
    }
    throw std::runtime_error("fixture acknowledgement did not settle");
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "private broker URL required");
        const std::string url = argv[1];
        require(url.starts_with("nats://127.0.0.1:"), "only a private loopback broker is allowed");
        altml::log::init("altml-test", "window-migration", "info");
        auto connection = altml::messaging::NatsClient::connect(url, "window-migration-fixture");
        require(bool(connection), "private broker unavailable");
        auto& client = **connection;
        jsStreamConfig stream; jsStreamConfig_Init(&stream);
        const char* subjects[] = {"alttech.>"};
        stream.Name = "WINDOW_INPUT"; stream.Subjects = subjects; stream.SubjectsLen = 1;
        stream.Storage = js_FileStorage; stream.MaxBytes = 4 * 1024 * 1024;
        stream.Duplicates = 120'000'000'000; stream.Replicas = 1;
        jsStreamInfo* stream_info = nullptr; jsErrCode code;
        require(js_AddStream(&stream_info, client.js(), &stream, nullptr, &code) == NATS_OK,
                "fixture stream creation failed");
        jsStreamInfo_Destroy(stream_info);
        jsConsumerConfig legacy; jsConsumerConfig_Init(&legacy);
        const char* filters[] = {"alttech.feature.>", "alttech.microfeature.venue.*.*.1m.v1"};
        legacy.Durable = "window-input"; legacy.FilterSubjects = filters; legacy.FilterSubjectsLen = 2;
        legacy.AckPolicy = js_AckExplicit; legacy.AckWait = 60'000'000'000;
        legacy.MaxDeliver = -1; legacy.MaxAckPending = 4096;
        jsConsumerInfo* info = nullptr;
        require(js_AddConsumer(&info, client.js(), stream.Name, &legacy, nullptr, &code) == NATS_OK,
                "fixture durable creation failed");
        jsConsumerInfo_Destroy(info);
        altml::config::ProjectConfig project;
        project.input_stream = "WINDOW_INPUT"; project.durable = "window-input";
        project.output_stream = "WINDOW_OUTPUT"; project.control_stream = "WINDOW_CONTROL";
        project.control_subjects = {"evt.window.>"};
        require(!altml::messaging::nats_provision(url, project), "ordinary startup silently widened filters");
        auto consumer = client.pull_consumer("WINDOW_INPUT", "window-input");
        require(bool(consumer), "fixture consumer bind failed");
        require(bool(client.js_publish("alttech.feature.fixture", "synthetic-test", "base-1")),
                "fixture publication failed");
        auto batch = consumer->fetch(1, 1000);
        require(bool(batch) && batch->size() == 1, "fixture base message missing");
        require(!altml::messaging::nats_provision(url, project, true), "inflight receipt permitted filter migration");
        require(bool(batch->front().ack()), "fixture acknowledgement failed");
        settled(client.js());
        const auto before = cursor(client.js());
        require(altml::messaging::nats_provision(url, project, true), "quiescent filter migration failed");
        const auto after = cursor(client.js());
        require(after.ack == before.ack && after.delivered == before.delivered && after.pending == 0,
                "filter migration changed the durable cursor");
        require(bool(client.js_publish("alttech.microfeature.venue.binance.TEST.5s.v1", "synthetic-ofi", "fast-1")),
                "fixture fast publication failed");
        batch = consumer->fetch(1, 1000);
        require(bool(batch) && batch->size() == 1 && batch->front().data() == "synthetic-ofi",
                "new fast source did not reach the existing durable");
        require(bool(batch->front().ack()), "fixture fast acknowledgement failed");
        settled(client.js());
        require(altml::messaging::nats_provision(url, project), "restart refused the migrated contract");
        require(altml::messaging::nats_provision(url, project, true), "idempotent migration failed");
        batch = consumer->fetch(1, 100);
        require(bool(batch) && batch->empty(), "filter migration replayed acknowledged data");
        std::cout << "PASS existing durable cursor preserved, inflight refused, fast source consumed, restart idempotent\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
