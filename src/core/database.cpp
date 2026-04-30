// Arda Yiğit - Hazani
// database.cpp — Local Package Database Implementation
// JSON-based local database that stores all installed package metadata.
// Supports CRUD operations, file ownership queries, and persistence.


#include "core/database.hpp"
#include "macman.hpp"
#include "cli/colors.hpp"
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>

namespace fs = std::filesystem;

namespace macman {

// --- Constructor ---

Database::Database(const std::string& db_path) 
    : db_path_(db_path.empty() ? get_local_db() : db_path) {}

// --- Ensure Required Directories Exist ---

bool Database::ensure_directories() const {
    try {
        fs::path db_dir = fs::path(db_path_).parent_path();
        if (!fs::exists(db_dir)) {
            fs::create_directories(db_dir);
        }
        
        // Also create sync db directory
        fs::path sync_dir(get_sync_db_dir());
        if (!fs::exists(sync_dir)) {
            fs::create_directories(sync_dir);
        }
        
        // Create cache directory
        fs::path cache_dir(get_cache_dir());
        if (!fs::exists(cache_dir)) {
            fs::create_directories(cache_dir);
        }
        
        return true;
    } catch (const std::exception&) {
        // Permission errors are expected when running without sudo
        // Directories will be created on first sudo run
        return false;
    }
}

// --- Load Database from Disk ---

bool Database::load() {
    if (!fs::exists(db_path_)) {
        // First run — no database yet, that's OK
        return true;
    }

    try {
        std::ifstream file(db_path_);
        if (!file.is_open()) {
            colors::print_error("Cannot open database: " + db_path_);
            return false;
        }

        nlohmann::json db_json;
        file >> db_json;

        packages_.clear();
        if (db_json.contains("packages") && db_json["packages"].is_object()) {
            for (auto& [name, pkg_json] : db_json["packages"].items()) {
                packages_[name] = Package::from_json(pkg_json);
            }
        }

        return true;
    } catch (const std::exception& e) {
        colors::print_error("Failed to parse database: " + std::string(e.what()));
        return false;
    }
}

// --- Save Database to Disk ---

bool Database::save() const {
    try {
        ensure_directories();

        nlohmann::json db_json;
        db_json["version"] = VERSION;
        db_json["package_count"] = packages_.size();
        
        nlohmann::json pkgs_json = nlohmann::json::object();
        for (const auto& [name, pkg] : packages_) {
            pkgs_json[name] = pkg.to_json();
        }
        db_json["packages"] = pkgs_json;

        std::ofstream file(db_path_);
        if (!file.is_open()) {
            colors::print_error("Cannot write database: " + db_path_);
            return false;
        }

        file << db_json.dump(2); // Pretty-printed with 2 spaces
        return true;
    } catch (const std::exception& e) {
        colors::print_error("Failed to save database: " + std::string(e.what()));
        return false;
    }
}

// --- Add Package ---

bool Database::add_package(const Package& pkg) {
    packages_[pkg.name] = pkg;
    return save();
}

// --- Remove Package ---

bool Database::remove_package(const std::string& name) {
    auto it = packages_.find(name);
    if (it == packages_.end()) {
        return false;
    }
    packages_.erase(it);
    return save();
}

// --- Update Package ---

bool Database::update_package(const Package& pkg) {
    if (packages_.find(pkg.name) == packages_.end()) {
        return false;
    }
    packages_[pkg.name] = pkg;
    return save();
}

// --- Check if Installed ---

bool Database::is_installed(const std::string& name) const {
    return packages_.find(name) != packages_.end();
}

// --- Get Single Package ---

std::optional<Package> Database::get_package(const std::string& name) const {
    auto it = packages_.find(name);
    if (it != packages_.end()) {
        return it->second;
    }
    return std::nullopt;
}

// --- Get All Packages ---

std::vector<Package> Database::get_all_packages() const {
    std::vector<Package> result;
    result.reserve(packages_.size());
    for (const auto& [name, pkg] : packages_) {
        result.push_back(pkg);
    }
    return result;
}

// --- Search Installed Packages ---

std::vector<Package> Database::search_installed(const std::string& query) const {
    std::vector<Package> results;
    std::string lower_query = query;
    std::transform(lower_query.begin(), lower_query.end(), lower_query.begin(), ::tolower);

    for (const auto& [name, pkg] : packages_) {
        std::string lower_name = name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
        
        std::string lower_desc = pkg.description;
        std::transform(lower_desc.begin(), lower_desc.end(), lower_desc.begin(), ::tolower);

        if (lower_name.find(lower_query) != std::string::npos ||
            lower_desc.find(lower_query) != std::string::npos) {
            results.push_back(pkg);
        }
    }
    return results;
}

// --- Find File Owner ---

std::string Database::find_owner(const std::string& filepath) const {
    for (const auto& [name, pkg] : packages_) {
        for (const auto& file : pkg.installed_files) {
            if (file == filepath) {
                return name;
            }
        }
    }
    return "";
}

// --- Get Files of Package ---

std::vector<std::string> Database::get_files(const std::string& pkg_name) const {
    auto it = packages_.find(pkg_name);
    if (it != packages_.end()) {
        return it->second.installed_files;
    }
    return {};
}

// --- Statistics ---

size_t Database::package_count() const {
    return packages_.size();
}

size_t Database::total_installed_size() const {
    size_t total = 0;
    for (const auto& [name, pkg] : packages_) {
        total += pkg.installed_size;
    }
    return total;
}

// --- Database Lock ---

bool Database::lock() {
    std::string lock_path = "/usr/local/var/run/macman.lock";
    
    // Ensure run directory exists
    try {
        fs::create_directories("/usr/local/var/run");
    } catch (...) {
        // Fallback to /tmp if no permission for /usr/local/var/run
        lock_path = "/tmp/macman.lock";
    }

    lock_fd_ = open(lock_path.c_str(), O_RDWR | O_CREAT, 0644);
    if (lock_fd_ == -1) {
        colors::print_error("Cannot create lock file: " + lock_path);
        return false;
    }

    if (flock(lock_fd_, LOCK_EX | LOCK_NB) == -1) {
        colors::print_error("Macman is already running in another process.");
        close(lock_fd_);
        lock_fd_ = -1;
        return false;
    }

    return true;
}

void Database::unlock() {
    if (lock_fd_ != -1) {
        flock(lock_fd_, LOCK_UN);
        close(lock_fd_);
        lock_fd_ = -1;
    }
}

} // namespace macman
