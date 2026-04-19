// aur_backend.hpp — Arch User Repository Source Builder
// Fallback backend for packages not available in Homebrew. Queries the
// AUR RPC API, downloads PKGBUILDs, extracts source URLs, and compiles
// from source using the system compiler (clang) to produce native Mach-O.
// Features:
// - In-memory search/info cache with TTL (5 min)
// - Self-healing build engine: detects common Linux→macOS errors and
// auto-patches source before retrying (up to MAX_BUILD_RETRIES)
// - 3-level macOS compatibility check (COMPATIBLE/PARTIAL/LINUX_ONLY)


#pragma once

#include "../core/package.hpp"
#include "../core/self_healing.hpp"
#include "../net/http_client.hpp"
#include <string>
#include <vector>
#include <optional>
#include <map>
#include <ctime>

namespace macman {

// --- PKGBUILD Parsed Data ---

struct PKGBUILDInfo {
    std::string pkgname;
    std::string pkgver;
    std::string pkgrel;
    std::string pkgdesc;
    std::string url;
    std::vector<std::string> source;        // Source URLs
    std::vector<std::string> depends;       // Runtime deps
    std::vector<std::string> makedepends;   // Build deps
    std::string build_commands;             // Extracted build() function
    std::string package_commands;           // Extracted package() function
};

// --- macOS Compatibility Level ---

enum class CompatLevel {
    COMPATIBLE,     // ✅ No known issues — proceed silently
    PARTIAL,        // ⚠️ Some Linux deps have macOS alternatives — warn & proceed
    LINUX_ONLY      // ❌ Hard Linux requirement — red warning + ask Y/n
};

// --- AUR Backend Class ---

class AURBackend {
public:
    AURBackend();
    ~AURBackend() = default;

    // --- Search ---

    std::vector<Package> search(const std::string& query);

    // --- Package Info ---

    std::optional<Package> get_info(const std::string& name);

    // --- Build from Source ---

    bool build_and_install(const std::string& name, const std::string& install_prefix,
                           std::vector<std::string>& installed_files);

    // --- Availability Check ---

    bool has_package(const std::string& name);

    // --- Compatibility Check (public for Transaction to call) ---

    CompatLevel check_macos_compatibility(const PKGBUILDInfo& info) const;
    std::string get_incompatibility_reason(const PKGBUILDInfo& info) const;

private:
    HttpClient http_;
    std::string build_dir_;
    SelfHealingEngine healing_engine_;

    // --- Result Cache (TTL-based) ---

    static constexpr int CACHE_TTL_SECONDS = 300;  // 5 minutes
    static constexpr int MAX_BUILD_RETRIES = 5;

    struct CachedPackage {
        Package pkg;
        time_t timestamp;
    };
    struct CachedSearchResult {
        std::vector<Package> results;
        time_t timestamp;
    };

    std::map<std::string, CachedPackage> info_cache_;
    std::map<std::string, CachedSearchResult> search_cache_;

    bool is_cache_valid(time_t timestamp) const;

    // --- Internal Helpers ---

    std::optional<PKGBUILDInfo> download_pkgbuild(const std::string& name);
    PKGBUILDInfo parse_pkgbuild(const std::string& pkgbuild_path) const;

    bool download_sources(const PKGBUILDInfo& info, const std::string& work_dir);
    bool compile_source(const PKGBUILDInfo& info, const std::string& work_dir,
                        const std::string& install_prefix,
                        std::vector<std::string>& installed_files);

    std::vector<std::string> collect_installed_files(const std::string& prefix) const;

    Package aur_json_to_package(const nlohmann::json& result) const;
};

} // namespace macman
