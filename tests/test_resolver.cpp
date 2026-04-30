#include <catch2/catch_all.hpp>
#include "core/resolver.hpp"
#include "core/database.hpp"
#include <filesystem>

using namespace macman;
namespace fs = std::filesystem;

TEST_CASE("Resolver logic", "[core][resolver]") {
    std::string test_db = "test_resolver.db";
    if (fs::exists(test_db)) fs::remove(test_db);

    Database db(test_db);
    REQUIRE(db.load());
    Resolver resolver(db);

    SECTION("Strip constraints") {
        REQUIRE(resolver.strip_constraint("sqlite>=3.45.0") == "sqlite");
        REQUIRE(resolver.strip_constraint("wget") == "wget");
        REQUIRE(resolver.strip_constraint("python<4.0") == "python");
    }

    SECTION("Orphan dependency detection") {
        Package pkg_a;
        pkg_a.name = "pkg-a";
        pkg_a.dependencies = {"dep-1"};
        pkg_a.install_reason = "explicit";
        
        Package dep_1;
        dep_1.name = "dep-1";
        dep_1.install_reason = "dependency";

        db.add_package(pkg_a);
        db.add_package(dep_1);

        // dep-1 is needed by pkg-a, so not an orphan of pkg-a yet? 
        // wait find_orphan_deps(pkg_name) finds orphans that WOULD BE created if pkg_name is removed.
        auto orphans = resolver.find_orphan_deps("pkg-a");
        REQUIRE(orphans.size() == 1);
        REQUIRE(orphans[0] == "dep-1");

        // If another package also depends on dep-1
        Package pkg_b;
        pkg_b.name = "pkg-b";
        pkg_b.dependencies = {"dep-1"};
        db.add_package(pkg_b);

        orphans = resolver.find_orphan_deps("pkg-a");
        REQUIRE(orphans.empty());
    }

    if (fs::exists(test_db)) fs::remove(test_db);
}
