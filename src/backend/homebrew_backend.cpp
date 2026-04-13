/*
 * ============================================================================
 *  homebrew_backend.cpp — Homebrew Formulae API Backend Implementation
 * ============================================================================
 *  Interfaces with Homebrew's public JSON API to search, fetch info, and
 *  download prebuilt bottles. Primary source for macOS packages.
 *
 *  Performance improvements over v1:
 *    - Pre-computed search index eliminates per-query tolower() overhead
 *    - Hash map name_index_ gives O(1) exact-match for get_info()
 *    - macOS version detection cached (was calling popen every time)
 *    - Lazy bottle URL resolution: only computed when actually downloading
 *    - Search results sorted by relevance (exact > prefix > substring)
 * ============================================================================
 */

#include "backend/homebrew_backend.hpp"
#include "macman.hpp"
#include "core/config.hpp"
#include "cli/colors.hpp"
#include "ui/progress_bar.hpp"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <array>

namespace fs = std::filesystem;

namespace macman {

// ─── Constructor ────────────────────────────────────────────────────────────

HomebrewBackend::HomebrewBackend()
    : cache_path_(get_sync_db_dir() + "/homebrew_formulae.json") {
    load_cache();
}

// ─── Detect macOS Version Codename (Cached) ─────────────────────────────────

std::string HomebrewBackend::detect_macos_version() const {
    // Return cached version if already detected
    if (!cached_macos_version_.empty()) {
        return cached_macos_version_;
    }

    // Run sw_vers to get macOS version
    std::array<char, 128> buffer;
    std::string version;
    
    FILE* pipe = popen("sw_vers -productVersion", "r");
    if (pipe) {
        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
            version += buffer.data();
        }
        pclose(pipe);
    }
    
    // Trim whitespace
    version.erase(version.find_last_not_of(" \n\r\t") + 1);

    std::string arch_prefix = "";
    FILE* arch_pipe = popen("uname -m", "r");
    if (arch_pipe) {
        if (fgets(buffer.data(), buffer.size(), arch_pipe) != nullptr) {
            std::string arch = buffer.data();
            if (arch.find("arm64") != std::string::npos) {
                arch_prefix = "arm64_";
            }
        }
        pclose(arch_pipe);
    }

    // Map version numbers to bottle tag names
    std::string result;
    if (version.find("15.") == 0) result = arch_prefix + "sequoia";
    else if (version.find("14.") == 0) result = arch_prefix + "sonoma";
    else if (version.find("13.") == 0) result = arch_prefix + "ventura";
    else if (version.find("12.") == 0) result = arch_prefix + "monterey";
    else if (version.find("11.") == 0) result = arch_prefix + "big_sur";
    else result = arch_prefix + "sonoma"; // Default fallback
    
    // Cache the result for future calls
    cached_macos_version_ = result;
    return result;
}

// ─── Build Search Index ─────────────────────────────────────────────────────
// Pre-computes lowercase names/descriptions and builds a hash map for O(1)
// exact-match lookups. Called once after cache load/refresh.

void HomebrewBackend::build_search_index() {
    name_index_.clear();
    search_index_.clear();

    if (!formula_cache_.is_array()) return;

    search_index_.reserve(formula_cache_.size());
    name_index_.reserve(formula_cache_.size());

    for (size_t i = 0; i < formula_cache_.size(); i++) {
        const auto& formula = formula_cache_[i];
        std::string name = formula.value("name", "");
        std::string desc = formula.value("desc", "");

        // Build hash map: name → cache index
        name_index_[name] = i;

        // Build pre-lowercased search entries
        SearchEntry entry;
        entry.lower_name = name;
        std::transform(entry.lower_name.begin(), entry.lower_name.end(), 
                       entry.lower_name.begin(), ::tolower);
        entry.lower_desc = desc;
        std::transform(entry.lower_desc.begin(), entry.lower_desc.end(), 
                       entry.lower_desc.begin(), ::tolower);
        search_index_.push_back(std::move(entry));
    }
}

// ─── Load Cached Formula List ───────────────────────────────────────────────

bool HomebrewBackend::load_cache() {
    if (!fs::exists(cache_path_)) return false;

    try {
        std::ifstream file(cache_path_);
        file >> formula_cache_;
        build_search_index();
        return true;
    } catch (...) {
        return false;
    }
}

// ─── Save Formula Cache ─────────────────────────────────────────────────────

bool HomebrewBackend::save_cache() const {
    try {
        fs::path dir = fs::path(cache_path_).parent_path();
        if (!fs::exists(dir)) {
            fs::create_directories(dir);
        }

        std::ofstream file(cache_path_);
        file << formula_cache_.dump();
        return true;
    } catch (...) {
        return false;
    }
}

// ─── Check if Cache is Recent ───────────────────────────────────────────────

bool HomebrewBackend::is_cache_fresh() const {
    if (!fs::exists(cache_path_)) return false;

    auto mod_time = fs::last_write_time(cache_path_);
    auto now = fs::file_time_type::clock::now();
    auto age = std::chrono::duration_cast<std::chrono::hours>(now - mod_time);
    
    return age.count() < 24; // Cache valid for 24 hours
}

// ─── Refresh Formula Database ───────────────────────────────────────────────

bool HomebrewBackend::refresh_formula_cache() {
    colors::print_action("Synchronizing package databases...");
    colors::print_substatus("Downloading homebrew formula list...");

    auto response = http_.get(BREW_FORMULA_LIST);
    if (!response.success) {
        colors::print_error("Failed to fetch formula list: " + response.error);
        return false;
    }

    try {
        formula_cache_ = nlohmann::json::parse(response.body);
        save_cache();
        build_search_index();
        colors::print_substatus("Formula database updated (" + 
                               std::to_string(formula_cache_.size()) + " packages)");
        return true;
    } catch (const std::exception& e) {
        colors::print_error("Failed to parse formula list: " + std::string(e.what()));
        return false;
    }
}

// ─── Refresh Cask Database ──────────────────────────────────────────────────

bool HomebrewBackend::refresh_cask_cache() {
    colors::print_substatus("Downloading homebrew cask list...");

    auto response = http_.get(BREW_CASK_LIST);
    if (!response.success) {
        colors::print_warning("Failed to fetch cask list (non-critical)");
        return false;
    }

    try {
        cask_cache_ = nlohmann::json::parse(response.body);
        
        // Save cask cache
        std::string cask_path = get_sync_db_dir() + "/homebrew_casks.json";
        std::ofstream file(cask_path);
        file << cask_cache_.dump();
        
        colors::print_substatus("Cask database updated (" + 
                               std::to_string(cask_cache_.size()) + " casks)");
        return true;
    } catch (...) {
        return false;
    }
}

// ─── Parse Formula JSON to Package ──────────────────────────────────────────

Package HomebrewBackend::parse_formula(const nlohmann::json& formula, bool resolve_bottle_url) const {
    Package pkg;
    
    pkg.name        = formula.value("name", "");
    pkg.description = formula.value("desc", "");
    pkg.homepage    = formula.value("homepage", "");
    pkg.source      = PackageSource::HOMEBREW;

    // Version info
    if (formula.contains("versions") && formula["versions"].is_object()) {
        pkg.version = formula["versions"].value("stable", "");
    }

    // Dependencies
    if (formula.contains("dependencies") && formula["dependencies"].is_array()) {
        for (const auto& dep : formula["dependencies"]) {
            if (dep.is_string()) {
                pkg.dependencies.push_back(dep.get<std::string>());
            }
        }
    }
    if (formula.contains("build_dependencies") && formula["build_dependencies"].is_array()) {
        for (const auto& dep : formula["build_dependencies"]) {
            if (dep.is_string()) {
                pkg.build_deps.push_back(dep.get<std::string>());
            }
        }
    }

    // Bottle URL — only resolve when needed (download time), skip during search
    if (resolve_bottle_url) {
        pkg.url = get_bottle_url(formula);
    }

    return pkg;
}

// ─── Get Bottle Download URL ────────────────────────────────────────────────

std::string HomebrewBackend::get_bottle_url(const nlohmann::json& formula) const {
    if (!formula.contains("bottle") || !formula["bottle"].is_object()) {
        return "";
    }

    const auto& bottle = formula["bottle"];
    if (!bottle.contains("stable") || !bottle["stable"].is_object()) {
        return "";
    }

    const auto& stable = bottle["stable"];
    std::string root_url = stable.value("root_url", "https://ghcr.io/v2/homebrew/core");
    
    if (stable.contains("files") && stable["files"].is_object()) {
        std::string macos_ver = detect_macos_version();
        const auto& files = stable["files"];

        // Try exact macOS version first
        if (files.contains(macos_ver)) {
            std::string sha = files[macos_ver].value("sha256", "");
            // Construct the GHCR URL
            std::string pkg_name = formula.value("name", "");
            return root_url + "/" + pkg_name + "/blobs/sha256:" + sha;
        }

        // Try "all" architecture
        if (files.contains("all")) {
            std::string sha = files["all"].value("sha256", "");
            std::string pkg_name = formula.value("name", "");
            return root_url + "/" + pkg_name + "/blobs/sha256:" + sha;
        }

        // Use whatever is available first
        for (auto& [key, val] : files.items()) {
            std::string sha = val.value("sha256", "");
            std::string pkg_name = formula.value("name", "");
            return root_url + "/" + pkg_name + "/blobs/sha256:" + sha;
        }
    }

    return "";
}

// ─── Search Packages (Indexed, Relevance-Sorted) ────────────────────────────

std::vector<Package> HomebrewBackend::search(const std::string& query) const {
    std::vector<Package> exact_matches;
    std::vector<Package> prefix_matches;
    std::vector<Package> contains_matches;
    
    if (!formula_cache_.is_array() || search_index_.empty()) return exact_matches;

    std::string lower_query = query;
    std::transform(lower_query.begin(), lower_query.end(), lower_query.begin(), ::tolower);

    for (size_t i = 0; i < search_index_.size(); i++) {
        const auto& entry = search_index_[i];
        
        // Exact name match → highest priority
        if (entry.lower_name == lower_query) {
            exact_matches.push_back(parse_formula(formula_cache_[i], false));
            continue;
        }

        // Prefix match on name → second priority
        if (entry.lower_name.find(lower_query) == 0) {
            prefix_matches.push_back(parse_formula(formula_cache_[i], false));
            if (prefix_matches.size() >= 20) continue; // Cap prefix matches
            continue;
        }

        // Substring match on name or description → third priority
        if (entry.lower_name.find(lower_query) != std::string::npos || 
            entry.lower_desc.find(lower_query) != std::string::npos) {
            contains_matches.push_back(parse_formula(formula_cache_[i], false));
        }

        // Total cap
        if (exact_matches.size() + prefix_matches.size() + contains_matches.size() >= 50) break;
    }

    // Merge results in relevance order
    std::vector<Package> results;
    results.reserve(exact_matches.size() + prefix_matches.size() + contains_matches.size());
    results.insert(results.end(), exact_matches.begin(), exact_matches.end());
    results.insert(results.end(), prefix_matches.begin(), prefix_matches.end());
    results.insert(results.end(), contains_matches.begin(), contains_matches.end());

    return results;
}

// ─── Get Info for a Package (O(1) via Hash Map) ─────────────────────────────

std::optional<Package> HomebrewBackend::get_info(const std::string& name) const {
    // O(1) lookup via hash map instead of linear scan
    auto it = name_index_.find(name);
    if (it != name_index_.end() && it->second < formula_cache_.size()) {
        return parse_formula(formula_cache_[it->second]);
    }
    return std::nullopt;
}

// ─── Get Info from Remote API ───────────────────────────────────────────────

std::optional<Package> HomebrewBackend::get_info_remote(const std::string& name) {
    std::string url = std::string(BREW_API_BASE) + "/formula/" + name + ".json";
    
    auto response = http_.get_json(url);
    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto formula = nlohmann::json::parse(response.body);
        return parse_formula(formula);
    } catch (...) {
        return std::nullopt;
    }
}

// ─── Download Bottle ────────────────────────────────────────────────────────

bool HomebrewBackend::download_bottle(const Package& pkg, const std::string& dest_path) {
    if (pkg.url.empty()) {
        colors::print_error("No bottle URL available for " + pkg.name);
        return false;
    }

    HttpClient http;
    http.set_timeout(DOWNLOAD_TIMEOUT_SECS);

    ProgressBar progress(pkg.name, pkg.download_size);

    return http.download_file(pkg.url, dest_path,
        [&progress](size_t total, size_t current, double speed) {
            if (total > 0) progress.set_total(total);
            progress.update(current, speed);
        });
}

// ─── Install Bottle ─────────────────────────────────────────────────────────

bool HomebrewBackend::install_bottle(const std::string& bottle_path, Package& pkg) {
    // Extract the bottle (tar.gz) to the prefix
    std::string extract_dir = get_prefix() + "/Cellar/" + pkg.name + "/" + pkg.version;
    
    // Create target directory
    if (!fs::exists(extract_dir)) {
        fs::create_directories(extract_dir);
    }

    // Extract the bottle archive
    std::string cmd = "tar xzf '" + bottle_path + "' -C '" + get_prefix() + "/Cellar/' 2>/dev/null";
    int ret = system(cmd.c_str());
    
    if (ret != 0) {
        // Try without Cellar path (some bottles have different structure)
        cmd = "tar xzf '" + bottle_path + "' -C '" + extract_dir + "/' --strip-components=2 2>/dev/null";
        ret = system(cmd.c_str());
        if (ret != 0) {
            colors::print_error("Failed to extract bottle for " + pkg.name);
            return false;
        }
    }

    // Create symlinks in bin, lib, include, share under macman prefix
    std::vector<std::string> link_dirs = {"bin", "lib", "include", "share"};
    for (const auto& dir : link_dirs) {
        std::string src_dir = extract_dir + "/" + dir;
        std::string dst_dir = get_prefix() + "/" + dir;
        
        if (fs::exists(src_dir)) {
            if (!fs::exists(dst_dir)) {
                fs::create_directories(dst_dir);
            }
            
            for (const auto& entry : fs::directory_iterator(src_dir)) {
                std::string link_path = dst_dir + "/" + entry.path().filename().string();
                try {
                    if (fs::exists(link_path) || fs::is_symlink(link_path)) {
                        fs::remove(link_path);
                    }
                    fs::create_symlink(entry.path(), link_path);
                    pkg.installed_files.push_back(link_path);
                } catch (const std::exception& e) {
                    colors::print_warning("Could not create link: " + link_path);
                }
            }
        }
    }

    return true;
}

// ─── Uninstall Package ──────────────────────────────────────────────────────

bool HomebrewBackend::uninstall(const Package& pkg) {
    // Remove Cellar directory
    std::string cellar_dir = get_prefix() + "/Cellar/" + pkg.name;
    
    if (fs::exists(cellar_dir)) {
        try {
            fs::remove_all(cellar_dir);
        } catch (const std::exception& e) {
            colors::print_error("Failed to remove: " + cellar_dir);
            return false;
        }
    }

    // Remove symlinks from well-known directories
    for (const auto& file : pkg.installed_files) {
        try {
            if (fs::exists(file) || fs::is_symlink(file)) {
                fs::remove(file);
            }
        } catch (...) {}
    }

    return true;
}

// ─── Check Package Availability ─────────────────────────────────────────────

bool HomebrewBackend::has_package(const std::string& name) const {
    // O(1) via hash map
    return name_index_.find(name) != name_index_.end();
}

} // namespace macman
