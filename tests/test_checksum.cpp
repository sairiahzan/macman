#include <catch2/catch_all.hpp>
#include "core/checksum.hpp"
#include <fstream>
#include <filesystem>

using namespace macman;
namespace fs = std::filesystem;

TEST_CASE("Checksum SHA-256 computation and verification", "[core][checksum]") {
    std::string test_file = "test_data.txt";
    std::string content = "Hello Macman!";
    // echo -n "Hello Macman!" | shasum -a 256
    // Output: f6bd4dddd8968fa7c70b55568491b6e6f434860f539534dad1e8f31aa14699a1
    std::string expected_hash = "f6bd4dddd8968fa7c70b55568491b6e6f434860f539534dad1e8f31aa14699a1";

    SECTION("Compute SHA-256 of a known file") {
        std::ofstream ofs(test_file);
        ofs << content;
        ofs.close();

        std::string actual_hash = Checksum::compute_sha256(test_file);
        REQUIRE(actual_hash == expected_hash);
        
        fs::remove(test_file);
    }

    SECTION("Verify SHA-256 of a known file") {
        std::ofstream ofs(test_file);
        ofs << content;
        ofs.close();

        bool is_valid = Checksum::verify_sha256(test_file, expected_hash);
        REQUIRE(is_valid == true);

        bool is_invalid = Checksum::verify_sha256(test_file, "wronghash");
        REQUIRE(is_invalid == false);

        fs::remove(test_file);
    }
}
