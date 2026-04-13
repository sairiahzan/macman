// database.hpp — Local Package Database Manager
// Manages the JSON-based local package database that tracks all installed
// packages, their versions, file lists, and install metadata. Supports
// add, remove, query, and list operations with file-locking for safety.


#pragma once

#include "package.hpp"
#include <string>
#include <vector>
#include <optional>
#include <map>

namespace macman {

class Database {
public:
    // --- Constructor / Destructor ---

    explicit Database(const std::string& db_path = "");
    ~Database() = default;

    // --- Database I/O ---

    bool load();
    bool save() const;
    bool ensure_directories() const;

    // --- Package CRUD Operations ---

    bool add_package(const Package& pkg);
    bool remove_package(const std::string& name);
    bool update_package(const Package& pkg);

    // --- Query Operations ---

    bool is_installed(const std::string& name) const;
    std::optional<Package> get_package(const std::string& name) const;
    std::vector<Package> get_all_packages() const;
    std::vector<Package> search_installed(const std::string& query) const;

    // --- File Ownership ---

    std::string find_owner(const std::string& filepath) const;
    std::vector<std::string> get_files(const std::string& pkg_name) const;

    // --- Statistics ---

    size_t package_count() const;
    size_t total_installed_size() const;

private:
    std::string db_path_;
    std::map<std::string, Package> packages_;
};

} // namespace macman
