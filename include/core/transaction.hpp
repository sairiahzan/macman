// Arda Yiğit - Hazani
// transaction.hpp — Install/Remove Transaction Handler [V1.2.0 Patch]
// Orchestrates package installation and removal with dependency resolution,
// user confirmation prompts, progress tracking, and rollback on failure.

#pragma once

#include "package.hpp"
#include "database.hpp"
#include "resolver.hpp"
#include "installer.hpp"
#include "remover.hpp"
#include <string>
#include <vector>

namespace macman {

enum class TransactionType {
    INSTALL,
    REMOVE,
    UPGRADE
};

class Transaction {
public:
    Transaction(Database& db);
    ~Transaction() = default;

    bool install(const std::string& pkg_name, bool as_dependency = false);
    bool install_multiple(const std::vector<std::string>& packages, 
                          TransactionType type = TransactionType::INSTALL);
    
    bool remove(const std::string& pkg_name, bool recursive = false);
    bool remove_multiple(const std::vector<std::string>& packages, bool recursive = false);
    bool remove_all();

    bool upgrade_all();
    bool refresh_databases();

    bool clean_cache();
    bool list_upgradable();

    void set_no_confirm(bool val) { no_confirm_ = val; }

private:
    Database& db_;
    Resolver resolver_;
    Installer installer_;
    Remover remover_;
    bool no_confirm_ = false;

    bool confirm_transaction(TransactionType type, 
                             const std::vector<Package>& packages,
                             size_t total_size) const;
};

} // namespace macman
