// transaction.hpp — Install/Remove Transaction Handler
// Orchestrates package installation and removal with dependency resolution,
// user confirmation prompts, progress tracking, and rollback on failure.


#pragma once

#include "package.hpp"
#include "database.hpp"
#include <string>
#include <vector>
#include <functional>

namespace macman {

// Forward declarations
class HomebrewBackend;
class AURBackend;

// --- Transaction Types ---

enum class TransactionType {
    INSTALL,
    REMOVE,
    UPGRADE
};

// --- Transaction Class ---

class Transaction {
public:
    Transaction(Database& db);
    ~Transaction() = default;

    // --- Install Operations ---

    bool install(const std::string& pkg_name, bool as_dependency = false);
    bool install_multiple(const std::vector<std::string>& packages);
    
    // --- Remove Operations ---

    bool remove(const std::string& pkg_name, bool recursive = false);
    bool remove_multiple(const std::vector<std::string>& packages, bool recursive = false);
    
    // Completely wipe all packages and clear cache
    bool remove_all();

    // --- Upgrade Operations ---

    bool upgrade_all();
    bool refresh_databases();

    // --- Configuration ---

    void set_no_confirm(bool val) { no_confirm_ = val; }

private:
    Database& db_;
    bool no_confirm_ = false;

    // --- Internal Helpers ---

    bool confirm_transaction(TransactionType type, 
                             const std::vector<Package>& packages,
                             size_t total_size) const;
    
    bool resolve_dependencies(const Package& pkg, 
                              std::vector<std::string>& resolved) const;
    
    std::vector<std::string> find_orphan_deps(const std::string& pkg_name) const;

    bool download_and_install(const Package& pkg, const std::string& reason);
    bool remove_package_files(const Package& pkg);
    
    Package resolve_package(const std::string& name) const;
};

} // namespace macman
