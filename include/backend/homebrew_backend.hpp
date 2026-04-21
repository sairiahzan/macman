// homebrew_backend.hpp — Homebrew Formulae API Integration [V1.2.0 Patch]
// Backend that interfaces with Homebrew's public JSON API to search,
// fetch info, and download prebuilt bottles for macOS packages.
// Primary package source — tried before AUR fallback.
// Performance: Uses pre-computed search index (hash map + lowercase cache)
// for O(1) exact-match and fast substring search without repeated
// string lowercasing at query time.


#pragma once

#include "../core/package.hpp"
#include "../net/http_client.hpp"
#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <nlohmann/json.hpp>

namespace macman {

class HomebrewBackend {
public:
    HomebrewBackend();
    ~HomebrewBackend() = default;

    // --- Database Sync ---

    bool refresh_formula_cache();
    bool refresh_cask_cache();
    bool is_cache_fresh() const;

    // --- Search ---

    std::vector<Package> search(const std::string& query) const;
    
    // --- Package Info ---

    std::optional<Package> get_info(const std::string& name) const;
    std::optional<Package> get_info_remote(const std::string& name);

    // --- Download ---

    bool download_bottle(const Package& pkg, const std::string& dest_path);
    
    // --- Install ---
    
    bool install_bottle(const std::string& bottle_path, const std::string& deploy_dir, std::vector<std::string>& installed_files);
    bool uninstall(const Package& pkg);

    // --- Package Availability Check ---

    bool has_package(const std::string& name) const;

private:
    HttpClient http_;
    nlohmann::json formula_cache_;
    nlohmann::json cask_cache_;
    std::string cache_path_;
    
    // --- Search Index (Performance) ---
    // Pre-computed at cache load time to avoid repeated work during search.

    // name → index into formula_cache_ array for O(1) exact-match lookup
    std::unordered_map<std::string, size_t> name_index_;

    // Pre-lowercased (name, description) pairs aligned with formula_cache_
    // Eliminates repeated tolower() calls during search
    struct SearchEntry {
        std::string lower_name;
        std::string lower_desc;
    };
    std::vector<SearchEntry> search_index_;

    // Cached macOS version string (detected once, reused everywhere)
    mutable std::string cached_macos_version_;

    // --- Internal Helpers ---

    Package parse_formula(const nlohmann::json& formula, bool resolve_bottle_url = true) const;
    std::string get_bottle_url(const nlohmann::json& formula) const;
    std::string detect_macos_version() const;
    
    bool load_cache();
    bool save_cache() const;
    void build_search_index();
};

} // namespace macman
