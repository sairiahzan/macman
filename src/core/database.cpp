#include "core/database.hpp"
#include "macman.hpp"
#include "cli/colors.hpp"
#include "core/logger.hpp"
#include <filesystem>
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include <set>

namespace fs = std::filesystem;

namespace macman {

Database::Database(const std::string& db_path) 
    : db_path_(db_path.empty() ? get_local_db() : db_path) {}

Database::~Database() {
    if (db_) {
        sqlite3_close(db_);
    }
    unlock();
}

bool Database::ensure_directories() const {
    try {
        fs::path db_dir = fs::path(db_path_).parent_path();
        if (!db_dir.empty() && !fs::exists(db_dir)) {
            fs::create_directories(db_dir);
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool Database::load() {
    ensure_directories();
    
    int rc = sqlite3_open(db_path_.c_str(), &db_);
    if (rc != SQLITE_OK) {
        colors::print_error("Failed to open SQLite database: " + std::string(sqlite3_errmsg(db_)));
        return false;
    }

    return init_schema();
}

bool Database::init_schema() {
    const char* sql_packages = 
        "CREATE TABLE IF NOT EXISTS packages ("
        "  name TEXT PRIMARY KEY,"
        "  version TEXT NOT NULL,"
        "  description TEXT,"
        "  homepage TEXT,"
        "  url TEXT,"
        "  sha256 TEXT,"
        "  installed_size INTEGER,"
        "  download_size INTEGER,"
        "  source TEXT,"
        "  install_date TEXT,"
        "  install_reason TEXT"
        ");";

    const char* sql_files = 
        "CREATE TABLE IF NOT EXISTS files ("
        "  pkg_name TEXT,"
        "  file_path TEXT,"
        "  sha256 TEXT,"
        "  PRIMARY KEY (pkg_name, file_path),"
        "  FOREIGN KEY (pkg_name) REFERENCES packages(name) ON DELETE CASCADE"
        ");";

    const char* sql_deps = 
        "CREATE TABLE IF NOT EXISTS dependencies ("
        "  pkg_name TEXT,"
        "  dep_name TEXT,"
        "  PRIMARY KEY (pkg_name, dep_name),"
        "  FOREIGN KEY (pkg_name) REFERENCES packages(name) ON DELETE CASCADE"
        ");";

    if (!execute_query(sql_packages)) return false;
    if (!execute_query(sql_files)) return false;
    if (!execute_query(sql_deps)) return false;

    return true;
}

bool Database::execute_query(const std::string& sql) const {
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        Logger::instance().error("SQL Error: " + std::string(err_msg));
        sqlite3_free(err_msg);
        return false;
    }
    return true;
}

bool Database::add_package(const Package& pkg) {
    sqlite3_stmt* stmt;
    const char* sql = "INSERT OR REPLACE INTO packages (name, version, description, homepage, url, sha256, "
                      "installed_size, download_size, source, install_date, install_reason) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, pkg.name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, pkg.version.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, pkg.description.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, pkg.homepage.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, pkg.url.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, pkg.sha256.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 7, pkg.installed_size);
    sqlite3_bind_int64(stmt, 8, pkg.download_size);
    sqlite3_bind_text(stmt, 9, source_to_string(pkg.source).c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 10, pkg.install_date.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 11, pkg.install_reason.c_str(), -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return false;
    }
    sqlite3_finalize(stmt);

    // Add files (both symlinks and real files with hashes)
    execute_query("DELETE FROM files WHERE pkg_name = '" + pkg.name + "';");
    
    // 1. Add all files from installed_files (mostly symlinks)
    std::set<std::string> all_recorded_files;
    for (const auto& f : pkg.installed_files) {
        std::string hash = "";
        auto it = pkg.file_hashes.find(f);
        if (it != pkg.file_hashes.end()) hash = it->second;

        sqlite3_stmt* f_stmt;
        const char* f_sql = "INSERT OR REPLACE INTO files (pkg_name, file_path, sha256) VALUES (?, ?, ?);";
        sqlite3_prepare_v2(db_, f_sql, -1, &f_stmt, nullptr);
        sqlite3_bind_text(f_stmt, 1, pkg.name.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(f_stmt, 2, f.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(f_stmt, 3, hash.c_str(), -1, SQLITE_STATIC);
        sqlite3_step(f_stmt);
        sqlite3_finalize(f_stmt);
        all_recorded_files.insert(f);
    }

    // 2. Add remaining files from file_hashes (real files in /opt)
    for (const auto& [path, hash] : pkg.file_hashes) {
        if (all_recorded_files.find(path) != all_recorded_files.end()) continue;

        sqlite3_stmt* f_stmt;
        const char* f_sql = "INSERT OR REPLACE INTO files (pkg_name, file_path, sha256) VALUES (?, ?, ?);";
        sqlite3_prepare_v2(db_, f_sql, -1, &f_stmt, nullptr);
        sqlite3_bind_text(f_stmt, 1, pkg.name.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(f_stmt, 2, path.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(f_stmt, 3, hash.c_str(), -1, SQLITE_STATIC);
        sqlite3_step(f_stmt);
        sqlite3_finalize(f_stmt);
    }

    // Add deps
    execute_query("DELETE FROM dependencies WHERE pkg_name = '" + pkg.name + "';");
    for (const auto& d : pkg.dependencies) {
        std::string d_sql = "INSERT INTO dependencies (pkg_name, dep_name) VALUES ('" + pkg.name + "', '" + d + "');";
        execute_query(d_sql);
    }

    return true;
}

bool Database::remove_package(const std::string& name) {
    std::string sql = "DELETE FROM packages WHERE name = '" + name + "';";
    return execute_query(sql);
}

bool Database::update_package(const Package& pkg) {
    return add_package(pkg); // SQLite INSERT OR REPLACE handles this
}

bool Database::is_installed(const std::string& name) const {
    sqlite3_stmt* stmt;
    const char* sql = "SELECT 1 FROM packages WHERE name = ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    
    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_STATIC);
    bool exists = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return exists;
}

std::optional<Package> Database::get_package(const std::string& name) const {
    sqlite3_stmt* stmt;
    const char* sql = "SELECT * FROM packages WHERE name = ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;

    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_STATIC);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        Package pkg;
        pkg.name = (const char*)sqlite3_column_text(stmt, 0);
        pkg.version = (const char*)sqlite3_column_text(stmt, 1);
        pkg.description = (const char*)sqlite3_column_text(stmt, 2);
        pkg.homepage = (const char*)sqlite3_column_text(stmt, 3);
        pkg.url = (const char*)sqlite3_column_text(stmt, 4);
        pkg.sha256 = (const char*)sqlite3_column_text(stmt, 5);
        pkg.installed_size = sqlite3_column_int64(stmt, 6);
        pkg.download_size = sqlite3_column_int64(stmt, 7);
        pkg.source = string_to_source((const char*)sqlite3_column_text(stmt, 8));
        pkg.install_date = (const char*)sqlite3_column_text(stmt, 9);
        pkg.install_reason = (const char*)sqlite3_column_text(stmt, 10);
        sqlite3_finalize(stmt);

        // Fetch files
        sqlite3_prepare_v2(db_, "SELECT file_path, sha256 FROM files WHERE pkg_name = ?", -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, pkg.name.c_str(), -1, SQLITE_STATIC);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string path = (const char*)sqlite3_column_text(stmt, 0);
            const char* hash_ptr = (const char*)sqlite3_column_text(stmt, 1);
            std::string hash = hash_ptr ? hash_ptr : "";
            
            pkg.installed_files.push_back(path);
            if (!hash.empty()) {
                pkg.file_hashes[path] = hash;
            }
        }
        sqlite3_finalize(stmt);

        // Fetch deps
        sqlite3_prepare_v2(db_, "SELECT dep_name FROM dependencies WHERE pkg_name = ?", -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, pkg.name.c_str(), -1, SQLITE_STATIC);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            pkg.dependencies.push_back((const char*)sqlite3_column_text(stmt, 0));
        }
        sqlite3_finalize(stmt);

        return pkg;
    }

    sqlite3_finalize(stmt);
    return std::nullopt;
}

std::vector<Package> Database::get_all_packages() const {
    std::vector<Package> results;
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_, "SELECT name FROM packages", -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        auto pkg = get_package((const char*)sqlite3_column_text(stmt, 0));
        if (pkg) results.push_back(*pkg);
    }
    sqlite3_finalize(stmt);
    return results;
}

std::vector<Package> Database::search_installed(const std::string& query) const {
    std::vector<Package> results;
    sqlite3_stmt* stmt;
    std::string sql = "SELECT name FROM packages WHERE name LIKE ? OR description LIKE ?;";
    sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    
    std::string like_query = "%" + query + "%";
    sqlite3_bind_text(stmt, 1, like_query.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, like_query.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        auto pkg = get_package((const char*)sqlite3_column_text(stmt, 0));
        if (pkg) results.push_back(*pkg);
    }
    sqlite3_finalize(stmt);
    return results;
}

std::string Database::find_owner(const std::string& filepath) const {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_, "SELECT pkg_name FROM files WHERE file_path = ?", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, filepath.c_str(), -1, SQLITE_STATIC);
    
    std::string owner = "";
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        owner = (const char*)sqlite3_column_text(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return owner;
}

std::vector<std::string> Database::get_files(const std::string& pkg_name) const {
    std::vector<std::string> files;
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_, "SELECT file_path FROM files WHERE pkg_name = ?", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, pkg_name.c_str(), -1, SQLITE_STATIC);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        files.push_back((const char*)sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return files;
}

size_t Database::package_count() const {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_, "SELECT count(*) FROM packages", -1, &stmt, nullptr);
    size_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

size_t Database::total_installed_size() const {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_, "SELECT sum(installed_size) FROM packages", -1, &stmt, nullptr);
    size_t total = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        total = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return total;
}

bool Database::lock() {
    std::string lock_path = "/usr/local/var/run/macman.lock";
    lock_fd_ = open(lock_path.c_str(), O_RDWR | O_CREAT, 0644);
    if (lock_fd_ == -1) {
        lock_path = "/tmp/macman.lock";
        lock_fd_ = open(lock_path.c_str(), O_RDWR | O_CREAT, 0644);
    }
    if (lock_fd_ == -1) return false;
    if (flock(lock_fd_, LOCK_EX | LOCK_NB) == -1) {
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
