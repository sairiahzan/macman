// transaction.cpp [V1.1.0 Patch]

#include "core/transaction.hpp"
#include "backend/homebrew_backend.hpp"
#include "backend/aur_backend.hpp"
#include "macman.hpp"
#include "cli/colors.hpp"
#include <iostream>
#include <iomanip>

namespace macman {

Transaction::Transaction(Database& db) 
    : db_(db), resolver_(db), installer_(db), remover_(db) {}

bool Transaction::confirm_transaction(TransactionType type,
                                       const std::vector<Package>& packages,
                                       size_t total_size) const {
    if (no_confirm_) return true;

    std::cout << std::endl;
    if (type == TransactionType::INSTALL) {
        std::cout << colors::BOLD_WHITE << "Packages (" << packages.size() << "): " << colors::RESET;
    } else if (type == TransactionType::REMOVE) {
        std::cout << colors::BOLD_WHITE << "Packages to remove (" << packages.size() << "): " << colors::RESET;
    } else {
        std::cout << colors::BOLD_WHITE << "Packages to upgrade (" << packages.size() << "): " << colors::RESET;
    }

    for (const auto& p : packages) {
        std::cout << p.name << "-" << p.version << " ";
        if (p.source == PackageSource::HOMEBREW) {
            std::cout << colors::BOLD_CYAN << "[macOS Native]" << colors::RESET << "  ";
        } else if (p.source == PackageSource::AUR) {
            std::cout << colors::BOLD_MAGENTA << "[Arch Linux AUR]" << colors::RESET << "  ";
        } else {
            std::cout << colors::DIM << "[Local]" << colors::RESET << "  ";
        }
    }
    std::cout << std::endl << std::endl;

    if (type == TransactionType::INSTALL || type == TransactionType::UPGRADE) {
        double mb = static_cast<double>(total_size) / (1024 * 1024);
        std::cout << colors::BOLD_WHITE << "Total Download Size: " << std::fixed << std::setprecision(2) << mb << " MiB" << colors::RESET << std::endl;
    }

    std::cout << colors::BOLD_BLUE << ":: " << colors::BOLD_WHITE << "Proceed with installation? [Y/n] " << colors::RESET;
    std::string input;
    std::getline(std::cin, input);

    return input.empty() || input == "Y" || input == "y" || input == "yes";
}

bool Transaction::install(const std::string& pkg_name, bool as_dependency) {
    (void)as_dependency;
    return install_multiple({pkg_name});
}

bool Transaction::install_multiple(const std::vector<std::string>& packages) {
    if (packages.empty()) return true;

    colors::print_action("Resolving packages concurrently...");
    auto unique_to_install = resolver_.resolve_all_concurrently(packages);
    if (unique_to_install.empty()) {
        colors::print_warning("No packages to install (or failed to resolve).");
        return false;
    }

    size_t total_size = 0;
    for (const auto& p : unique_to_install) {
        total_size += p.download_size;
    }

    if (!confirm_transaction(TransactionType::INSTALL, unique_to_install, total_size)) {
        std::cout << "Transaction cancelled." << std::endl;
        return false;
    }

    bool all_success = true;
    std::vector<Package> installed_in_this_transaction;

    for (const auto& pkg : unique_to_install) {
        colors::print_status("Installing " + pkg.name + " " + pkg.version + "...");
        
        bool is_explicit = false;
        for (const auto& t : packages) {
            if (t == pkg.name) { is_explicit = true; break; }
        }
        
        if (!installer_.install_package(pkg, is_explicit ? "explicit" : "dependency")) {
            colors::print_error("Failed to install: " + pkg.name);
            all_success = false;
            
            // Transaction Rollback: Cleanup packages installed in this batch if one fails
            colors::print_warning("Installation failed. Rolling back transaction (" + 
                                  std::to_string(installed_in_this_transaction.size()) + " packages)...");
            
            // Reverse order rollback
            for (auto it = installed_in_this_transaction.rbegin(); it != installed_in_this_transaction.rend(); ++it) {
                remover_.remove_package(*it);
            }
            colors::print_substatus("Dependencies removed.");
            
            break; // Stop on first failure in batch
        }
        
        installed_in_this_transaction.push_back(pkg);
        colors::print_success(pkg.name + " " + pkg.version + " installed successfully");
    }

    return all_success;
}

bool Transaction::remove(const std::string& pkg_name, bool recursive) {
    return remove_multiple({pkg_name}, recursive);
}

bool Transaction::remove_multiple(const std::vector<std::string>& packages, bool recursive) {
    bool all_success = true;
    std::vector<Package> to_remove;
    
    for (const auto& pkg_name : packages) {
        auto pkg = db_.get_package(pkg_name);
        if (!pkg) {
            colors::print_error("target not found: " + pkg_name);
            all_success = false;
            continue;
        }
        to_remove.push_back(*pkg);

        if (recursive) {
            auto orphans = resolver_.find_orphan_deps(pkg_name);
            for (const auto& o : orphans) {
                auto orphan_pkg = db_.get_package(o);
                if (orphan_pkg) to_remove.push_back(*orphan_pkg);
            }
        }
    }

    if (to_remove.empty()) return all_success;

    if (!confirm_transaction(TransactionType::REMOVE, to_remove, 0)) {
        std::cout << "Transaction cancelled." << std::endl;
        return false;
    }

    for (const auto& pkg : to_remove) {
        if (!remover_.remove_package(pkg)) {
            all_success = false;
        }
    }

    return all_success;
}

bool Transaction::remove_all() {
    return remover_.nuke_system();
}

bool Transaction::refresh_databases() {
    HomebrewBackend brew;
    bool success = brew.refresh_formula_cache();
    brew.refresh_cask_cache(); // Best effort
    return success;
}

bool Transaction::upgrade_all() {
    colors::print_action("Starting full system upgrade...");

    if (!refresh_databases()) {
        colors::print_error("Failed to synchronize package databases");
        return false;
    }

    auto installed = db_.get_all_packages();
    std::vector<Package> packages_to_upgrade;

    for (const auto& pkg : installed) {
        Package latest_pkg = resolver_.resolve_package(pkg.name);
        if (!latest_pkg.version.empty() && 
            Package::compare_versions(latest_pkg.version, pkg.version) > 0) {
            packages_to_upgrade.push_back(latest_pkg);
        }
    }

    if (packages_to_upgrade.empty()) {
        std::cout << " there is nothing to do" << std::endl;
        return true;
    }

    size_t total_size = 0;
    for (const auto& p : packages_to_upgrade) total_size += p.download_size;

    if (!confirm_transaction(TransactionType::UPGRADE, packages_to_upgrade, total_size)) {
        std::cout << "Transaction cancelled." << std::endl;
        return false;
    }

    bool all_success = true;
    for (const auto& pkg : packages_to_upgrade) {
        colors::print_status("Upgrading " + pkg.name + " to " + pkg.version + "...");
        if (!installer_.install_package(pkg, pkg.install_reason)) {
            colors::print_error("Failed to upgrade: " + pkg.name);
            all_success = false;
        } else {
            colors::print_success(pkg.name + " upgraded successfully");
        }
    }

    return all_success;
}

} // namespace macman
