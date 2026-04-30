#include <catch2/catch_all.hpp>
#include "core/database.hpp"
#include <filesystem>
#include <fstream>

using namespace macman;
namespace fs = std::filesystem;

TEST_CASE("Database operations", "[core][database]") {
    std::string test_db = "test_macman.db";
    if (fs::exists(test_db)) fs::remove(test_db);

    Database db(test_db);
    db.ensure_directories();
    REQUIRE(db.load());

    Package pkg;
    pkg.name = "test-pkg";
    pkg.version = "1.0.0";
    pkg.description = "A test package";
    pkg.source = PackageSource::LOCAL;
    pkg.installed_files = {"/tmp/test1", "/tmp/test2"};

    SECTION("Add and retrieve package") {
        REQUIRE(db.add_package(pkg));
        REQUIRE(db.is_installed("test-pkg"));
        
        auto retrieved = db.get_package("test-pkg");
        REQUIRE(retrieved.has_value());
        REQUIRE(retrieved->version == "1.0.0");
        REQUIRE(retrieved->installed_files.size() == 2);
    }

    SECTION("Update package") {
        db.add_package(pkg);
        pkg.version = "1.1.0";
        REQUIRE(db.update_package(pkg));
        
        auto retrieved = db.get_package("test-pkg");
        REQUIRE(retrieved->version == "1.1.0");
    }

    SECTION("Search packages") {
        db.add_package(pkg);
        auto results = db.search_installed("test");
        REQUIRE(results.size() >= 1);
        REQUIRE(results[0].name == "test-pkg");
    }

    SECTION("Find file owner") {
        db.add_package(pkg);
        REQUIRE(db.find_owner("/tmp/test1") == "test-pkg");
        REQUIRE(db.find_owner("/nonexistent") == "");
    }

    SECTION("Remove package") {
        db.add_package(pkg);
        REQUIRE(db.remove_package("test-pkg"));
        REQUIRE_FALSE(db.is_installed("test-pkg"));
    }

    SECTION("Database locking") {
        REQUIRE(db.lock());
        
        // Attempt to lock again from same object should fail or handle gracefully
        // Actually flock is per-process, so same process can often re-lock.
        // But our implementation returns false if flock fails.
        
        db.unlock();
    }

    if (fs::exists(test_db)) fs::remove(test_db);
    if (fs::exists("/tmp/macman.lock")) fs::remove("/tmp/macman.lock");
}
