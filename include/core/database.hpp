// Arda Yiğit - Hazani
// database.hpp — SQLite-based Local Package Database Manager
// Manages the SQLite local package database that tracks all installed
// packages, their versions, file lists, and install metadata.

#pragma once

#include "package.hpp"
#include <string>
#include <vector>
#include <optional>
#include <sqlite3.h>

namespace macman {

class Database {
public:
    explicit Database(const std::string& db_path = "");
    ~Database();

    // Prevent copying
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // --- Database Initialization ---
    bool load(); // Now initializes SQLite connection and schema
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

    // --- Database Lock ---
    bool lock();
    void unlock();

private:
    std::string db_path_;
    sqlite3* db_ = nullptr;
    int lock_fd_ = -1;

    bool init_schema();
    bool execute_query(const std::string& sql) const;
};

} // namespace macman
