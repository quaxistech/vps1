#include "altmacro/components/publisher.hpp"
#include "altmacro/core/nats_provision.hpp"
#include <iostream>

int main(int argc, char** argv) {
    if (argc < 2 || !std::string(argv[1]).starts_with("nats://127.0.0.1:")) return 2;
    altmacro::NatsConfig cfg;
    cfg.url = argv[1]; cfg.publish_timeout_ms = 200;
    if (!altmacro::messaging::nats_provision(cfg.url, cfg)) return 3;
    auto connection = altmacro::messaging::NatsClient::connect(cfg.url, "local-publication-fixture");
    if (!connection) return 4;
    altmacro::components::Publisher publisher(cfg, connection->get());
    altmacro::data::SourceBinding binding;
    binding.instrument_code = "FIXTURE.RATE"; binding.provider_code = "fixture";
    binding.native_frequency = "daily";
    altmacro::domain::ReferenceValue value;
    value.event_time_ms = 1000; value.publication_time_ms = 1100; value.value = 1.0;
    const std::string mode = argc > 2 ? argv[2] : "normal";
    if (mode == "outage") {
        std::cout << "READY" << std::endl;
        std::string trigger; std::getline(std::cin, trigger);
        const auto result = publisher.publish_reference(binding, {value});
        return result ? 5 : 0;
    }
    if (!publisher.publish_reference(binding, {value})) return 6;
    if (!publisher.publish_reference(binding, {value})) return 7;
    value.value = 2.0;
    if (!publisher.publish_reference(binding, {value})) return 8;
    value.quality = altmacro::domain::QualityStatus::partial;
    if (!publisher.publish_reference(binding, {value})) return 9;
    if (mode == "new") {
        value.value = 3.0;
        if (!publisher.publish_reference(binding, {value})) return 10;
    }
    return 0;
}
