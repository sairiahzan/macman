#include <catch2/catch_all.hpp>
#include "core/config.hpp"
#include <filesystem>
#include <fstream>

using namespace macman;
namespace fs = std::filesystem;

TEST_CASE("Config system operations", "[core][config]") {
    std::string test_config = "test_macman.conf";
    auto& config = Config::instance();

    SECTION("Default values are set correctly") {
        config.create_default(test_config);
        config.load(test_config);
        
        // Check some defaults (adjust based on actual defaults in src/core/config.cpp)
        REQUIRE(!config.get_cache_dir().empty());
        REQUIRE(config.get_parallel_downloads() > 0);
        
        fs::remove(test_config);
    }

    SECTION("Setting and getting values") {
        config.set("test_key", "test_value");
        REQUIRE(config.get("test_key") == "test_value");
        REQUIRE(config.get("non_existent", "default") == "default");
    }

    SECTION("Save and load config") {
        config.set("save_test", "123");
        config.save(test_config);
        
        config.set("save_test", "000"); // change it in memory
        config.load(test_config);
        
        REQUIRE(config.get("save_test") == "123");
        fs::remove(test_config);
    }
}
