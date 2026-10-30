#include "altsignalgateway/core/codec.hpp"
#include "altsignalgateway/domain/validation.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace altsignalgateway;

TEST_CASE("Gateway validates the exact finals recovered by the actual Quality evaluator") {
    const char* directory=std::getenv("ALTSQ_TEST_PUBLICATION_EVIDENCE_DIR");
    if(!directory) SKIP("run the isolated Quality publication fixture first");
    for(const auto* name:{"duplicate-cold","duplicate-scored","restart-cold","restart-scored","restart-fresh"}) {
        CAPTURE(name);
        std::ifstream file(std::filesystem::path(directory)/(std::string(name)+".bin"),std::ios::binary);
        REQUIRE(file);
        std::ostringstream bytes; bytes<<file.rdbuf();
        const auto decoded=messaging::parse_upstream_envelope(bytes.str());
        REQUIRE(decoded);
        const auto envelope=domain::validate_envelope(*decoded);
        INFO(envelope.detail);
        REQUIRE(envelope.ok());
        const auto& final=decoded->final_trade_signal();
        const auto signal=domain::validate_final_signal(final,final.proposal().created_at_ms()+1);
        INFO(signal.detail);
        CHECK(signal.ok());
        CHECK(final.quality().has_quality_score()==std::string_view(name).ends_with("scored"));
        CHECK_FALSE(final.proposal().recommended_for_execution());
    }
}
