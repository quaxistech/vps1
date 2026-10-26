#include "altsignalquality/core/verified_artifact.hpp"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <unistd.h>
using namespace altsignalquality;

TEST_CASE("verified model buffers survive pathname replacement and reject corrupt text") {
    char directory[] = "/tmp/altsignalquality-artifact-XXXXXX";
    REQUIRE(mkdtemp(directory) != nullptr);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::filesystem::remove_all(path); }
    } cleanup{directory};
    const auto path = cleanup.path / "model.txt";
    const auto write = [&](const std::string& bytes) {
        std::ofstream file(path, std::ios::binary);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    };
    const std::string original = "verified model A\n";
    write(original);
    const auto buffer = verified_model_bytes(path.string(), sha256_hex(original));
    REQUIRE(buffer);
    write("replacement B\n");
    CHECK(*buffer == original);
    CHECK_FALSE(verified_model_bytes(path.string(), sha256_hex(original)));
    write("");
    CHECK_FALSE(verified_model_bytes(path.string(), sha256_hex("")));
    const std::string with_nul("model\0suffix", 12);
    write(with_nul);
    CHECK_FALSE(verified_model_bytes(path.string(), sha256_hex(with_nul)));
    {
        std::ofstream sparse(path, std::ios::binary);
        sparse.seekp(128 * 1024 * 1024);
        sparse.put('x');
    }
    CHECK_FALSE(verified_model_bytes(path.string(), sha256_hex(original)));
    std::filesystem::remove(path);
    CHECK_FALSE(verified_model_bytes(path.string(), sha256_hex(original)));
}
