/*
 * ============================================================================
 *  transaction.cpp — Install/Remove Transaction Handler Implementation
 * ============================================================================
 *  Orchestrates full install/remove/upgrade workflows with dependency
 *  resolution, user prompts, download, extraction, and database updates.
 * ============================================================================
 */

#include "core/transaction.hpp"
#include "backend/homebrew_backend.hpp"
#include "backend/aur_backend.hpp"
#include "net/downloader.hpp"
#include "macman.hpp"
#include "cli/colors.hpp"
#include <iostream>
#include <algorithm>
#include <iomanip>
#include <ctime>
#include <filesystem>
#include <future>

namespace fs = std::filesystem;

namespace macman {

// ─── Constructor ────────────────────────────────────────────────────────────

Transaction::Transaction(Database& db) : db_(db) {}

// ─── Resolve Package (Homebrew first, then AUR) ─────────────────────────────

Package Transaction::resolve_package(const std::string& name) const {
    // Try Homebrew cache first (O(1) via hash map)
    HomebrewBackend brew;
    auto pkg = brew.get_info(name);
    if (pkg) return *pkg;

    // Not in local cache -> Fetch from remotes concurrently
    auto brew_future = std::async(std::launch::async, [&brew, name]() {
        return brew.get_info_remote(name);
    });

    auto aur_future = std::async(std::launch::async, [name]() {
        AURBackend aur;
        return aur.get_info(name);
    });

    auto brew_pkg_remote = brew_future.get();
    if (brew_pkg_remote) return *brew_pkg_remote;

    auto aur_pkg = aur_future.get();
    if (aur_pkg) return *aur_pkg;

    // Not found
    Package empty;
    empty.name = name;
    return empty;
}

// ─── User Confirmation Prompt ───────────────────────────────────────────────

bool Transaction::confirm_transaction(TransactionType type,
                                       const std::vector<Package>& packages,
                                       size_t total_size) const {
    if (no_confirm_) return true;

    std::cout << std::endl;

    // Show summary
    std::string action;
    switch (type) {
        case TransactionType::INSTALL: action = "Packages"; break;
        case TransactionType::REMOVE:  action = "Remove"; break;
        case TransactionType::UPGRADE: action = "Upgrade"; break;
    }

    std::cout << colors::BOLD_WHITE << action << " (" << packages.size() << "):" << colors::RESET << " ";
    for (size_t i = 0; i < packages.size(); i++) {
        if (i > 0) std::cout << "  ";
        
        std::string source_tag;
        if (packages[i].source == PackageSource::HOMEBREW) {
            source_tag = std::string(colors::BOLD_CYAN) + "[macOS Native]" + colors::RESET;
        } else if (packages[i].source == PackageSource::AUR) {
            source_tag = std::string(colors::BOLD_MAGENTA) + "[Arch Linux AUR]" + colors::RESET;
        } else {
            source_tag = std::string(colors::DIM) + "[Local]" + colors::RESET;
        }

        std::cout << packages[i].name << "-" << packages[i].version << " " << source_tag;
    }
    std::cout << std::endl << std::endl;

    if (total_size > 0 && type != TransactionType::REMOVE) {
        Package fmt;
        std::cout << "Total Download Size:  " << fmt.format_size(total_size) << std::endl;
    }
    
    std::cout << std::endl;
    std::cout << colors::BOLD_BLUE << ":: " << colors::BOLD_WHITE 
              << "Proceed with " 
              << (type == TransactionType::REMOVE ? "removal" : "installation") 
              << "? [Y/n] " << colors::RESET;

    std::string input;
    std::getline(std::cin, input);

    return input.empty() || input == "Y" || input == "y" || input == "yes";
}

// ─── Resolve Dependencies ──────────────────────────────────────────────────

bool Transaction::resolve_dependencies(const Package& pkg, 
                                        std::vector<std::string>& resolved) const {
    for (const auto& dep : pkg.dependencies) {
        // Skip if already installed or already in the resolve list
        if (db_.is_installed(dep)) continue;
        if (std::find(resolved.begin(), resolved.end(), dep) != resolved.end()) continue;

        resolved.push_back(dep);
        
        // Recursively resolve sub-dependencies
        Package dep_pkg = resolve_package(dep);
        if (!dep_pkg.version.empty()) {
            resolve_dependencies(dep_pkg, resolved);
        }
    }
    return true;
}

// ─── Find Orphaned Dependencies ─────────────────────────────────────────────

std::vector<std::string> Transaction::find_orphan_deps(const std::string& pkg_name) const {
    std::vector<std::string> orphans;
    
    auto pkg_opt = db_.get_package(pkg_name);
    if (!pkg_opt) return orphans;

    for (const auto& dep : pkg_opt->dependencies) {
        auto dep_pkg = db_.get_package(dep);
        if (!dep_pkg) continue;
        if (dep_pkg->install_reason != "dependency") continue;

        // Check if any other installed package depends on this
        bool is_needed = false;
        for (const auto& other : db_.get_all_packages()) {
            if (other.name == pkg_name) continue;
            for (const auto& other_dep : other.dependencies) {
                if (other_dep == dep) {
                    is_needed = true;
                    break;
                }
            }
            if (is_needed) break;
        }

        if (!is_needed) {
            orphans.push_back(dep);
        }
    }

    return orphans;
}

// ─── Download and Install a Single Package ──────────────────────────────────

bool Transaction::download_and_install(const Package& pkg, const std::string& reason) {
    Package installed = pkg;
    installed.install_reason = reason;

    // Set install date
    auto now = std::time(nullptr);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
    installed.install_date = buf;

    if (pkg.source == PackageSource::AUR) {
        // Build from AUR source
        colors::print_status("Sources found in Arch Linux AUR. Compiling natively for macOS...");
        AURBackend aur;
        if (!aur.build_and_install(pkg.name, get_prefix(), installed.installed_files)) {
            return false;
        }
    } else {
        // Download from Homebrew
        if (pkg.url.empty()) {
            colors::print_error("No download URL for " + pkg.name);
            return false;
        }

        std::string bottle_path = get_cache_dir() + "/" + pkg.name + "-" + pkg.version + ".tar.gz";

        Downloader dl;
        DownloadTask task;
        task.url = pkg.url;
        task.output_path = bottle_path;
        task.label = pkg.name;
        task.expected_size = pkg.download_size;

        auto result = dl.download(task);
        if (!result.success) {
            colors::print_error("Failed to download " + pkg.name);
            return false;
        }

        // Install the bottle
        colors::print_substatus("Found native macOS binary in Homebrew. Installing " + pkg.name + "...");
        HomebrewBackend brew;
        if (!brew.install_bottle(bottle_path, installed)) {
            return false;
        }
    }

    db_.add_package(installed);
    
    return true;
}

// ─── Remove Package Files ──────────────────────────────────────────────────

bool Transaction::remove_package_files(const Package& pkg) {
    if (pkg.source == PackageSource::HOMEBREW) {
        HomebrewBackend brew;
        return brew.uninstall(pkg);
    } else {
        // Smart Uninstaller for AUR / Containerized files
        int files_removed = 0;
        int dirs_removed = 0;
        std::string prefix = get_prefix();
        
        for (const auto& file : pkg.installed_files) {
            try {
                if (fs::exists(file) || fs::is_symlink(file)) {
                    if (!fs::is_directory(file)) {
                        fs::remove(file);
                        files_removed++;
                        
                        // Try to clean up empty parent directories up to prefix
                        fs::path parent = fs::path(file).parent_path();
                        while (parent.string() != prefix && 
                               parent.string().length() > prefix.length() &&
                               fs::exists(parent) && fs::is_empty(parent)) {
                            fs::remove(parent);
                            dirs_removed++;
                            parent = parent.parent_path();
                        }
                    }
                }
            } catch (...) {}
        }
        
        colors::print_substatus("Removed " + std::to_string(files_removed) + " files and " + 
                              std::to_string(dirs_removed) + " empty directories.");
        return true;
    }
}

// ─── Install Package(s) ────────────────────────────────────────────────────

bool Transaction::install(const std::string& pkg_name, bool as_dependency) {
    // Check if already installed
    if (db_.is_installed(pkg_name)) {
        colors::print_warning(pkg_name + " is already installed -- reinstalling");
    }

    // Resolve the package
    colors::print_action("Resolving dependencies...");
    Package pkg = resolve_package(pkg_name);
    
    if (pkg.version.empty()) {
        colors::print_error("target not found: " + pkg_name);
        return false;
    }

    // Resolve dependencies
    std::vector<std::string> dep_names;
    resolve_dependencies(pkg, dep_names);

    // Build the list of packages to install
    std::vector<Package> to_install;
    for (const auto& dep : dep_names) {
        to_install.push_back(resolve_package(dep));
    }
    to_install.push_back(pkg);

    // Calculate total size
    size_t total_size = 0;
    for (const auto& p : to_install) {
        total_size += p.download_size;
    }

    // Confirm
    if (!confirm_transaction(TransactionType::INSTALL, to_install, total_size)) {
        std::cout << "Transaction cancelled." << std::endl;
        return false;
    }

    // Install dependencies first
    for (const auto& dep_name : dep_names) {
        if (db_.is_installed(dep_name)) continue;
        
        Package dep_pkg = resolve_package(dep_name);
        if (dep_pkg.version.empty()) {
            colors::print_warning("Dependency not found: " + dep_name + " (skipping)");
            continue;
        }

        colors::print_status("Installing dependency: " + dep_name);
        if (!download_and_install(dep_pkg, "dependency")) {
            colors::print_error("Failed to install dependency: " + dep_name);
            return false;
        }
    }

    // Install the main package
    colors::print_status("Installing " + pkg.name + " " + pkg.version + "...");
    if (!download_and_install(pkg, as_dependency ? "dependency" : "explicit")) {
        return false;
    }

    colors::print_success(pkg.name + " " + pkg.version + " installed successfully");
    return true;
}

bool Transaction::install_multiple(const std::vector<std::string>& packages) {
    if (packages.empty()) return true;

    colors::print_action("Resolving packages concurrently...");
    std::vector<std::future<Package>> futures;
    for (const auto& pkg_name : packages) {
        // Launch package resolution in parallel
        futures.push_back(std::async(std::launch::async, [this, pkg_name]() {
            return resolve_package(pkg_name);
        }));
    }

    std::vector<Package> target_pkgs;
    bool all_found = true;
    for (size_t i = 0; i < futures.size(); ++i) {
        Package pkg = futures[i].get();
        if (pkg.version.empty()) {
            colors::print_error("target not found: " + packages[i]);
            all_found = false;
        } else {
            target_pkgs.push_back(pkg);
        }
    }
    if (!all_found) return false;

    std::vector<std::string> all_dep_names;
    for (const auto& pkg : target_pkgs) {
        resolve_dependencies(pkg, all_dep_names);
    }

    std::vector<Package> to_install;
    if (!all_dep_names.empty()) {
        std::vector<std::future<Package>> dep_futures;
        for (const auto& dep_name : all_dep_names) {
            dep_futures.push_back(std::async(std::launch::async, [this, dep_name]() {
                return resolve_package(dep_name);
            }));
        }
        for (size_t i = 0; i < dep_futures.size(); ++i) {
            Package dep = dep_futures[i].get();
            if (dep.version.empty()) {
                colors::print_warning("Dependency not found: " + all_dep_names[i] + " (skipping)");
            } else {
                to_install.push_back(dep);
            }
        }
    }

    // Append main targets after their dependencies
    to_install.insert(to_install.end(), target_pkgs.begin(), target_pkgs.end());

    // Deduplicate (in case multiple targets share a dependency or target is repeated)
    std::vector<Package> unique_to_install;
    for (const auto& p : to_install) {
        bool duplicate = false;
        for (const auto& u : unique_to_install) {
            if (u.name == p.name) { duplicate = true; break; }
        }
        if (!duplicate && !db_.is_installed(p.name)) {
            unique_to_install.push_back(p);
        }
    }

    if (unique_to_install.empty()) {
        colors::print_warning("All requested packages and their dependencies are already installed.");
        return true;
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
    for (const auto& pkg : unique_to_install) {
        colors::print_status("Installing " + pkg.name + " " + pkg.version + "...");
        bool is_explicit = false;
        for (const auto& t : target_pkgs) {
            if (t.name == pkg.name) { is_explicit = true; break; }
        }
        
        if (!download_and_install(pkg, is_explicit ? "explicit" : "dependency")) {
            colors::print_error("Failed to install: " + pkg.name);
            all_success = false;
            break; // Stop on first failure in batch
        }
        colors::print_success(pkg.name + " " + pkg.version + " installed successfully");
    }

    return all_success;
}

// ─── Remove Package ────────────────────────────────────────────────────────

bool Transaction::remove(const std::string& pkg_name, bool recursive) {
    auto pkg = db_.get_package(pkg_name);
    if (!pkg) {
        colors::print_error("target not found: " + pkg_name);
        return false;
    }

    std::vector<Package> to_remove = {*pkg};
    
    // Find orphan dependencies if recursive
    std::vector<std::string> orphans;
    if (recursive) {
        orphans = find_orphan_deps(pkg_name);
        for (const auto& orphan_name : orphans) {
            auto orphan_pkg = db_.get_package(orphan_name);
            if (orphan_pkg) {
                to_remove.push_back(*orphan_pkg);
            }
        }
    }

    // Confirm removal
    if (!confirm_transaction(TransactionType::REMOVE, to_remove, 0)) {
        std::cout << "Transaction cancelled." << std::endl;
        return false;
    }

    // Remove the package
    colors::print_status("Removing " + pkg_name + "...");
    
    if (!remove_package_files(*pkg)) {
        colors::print_warning("Some files could not be removed");
    }
    db_.remove_package(pkg_name);

    // Remove orphan dependencies
    for (const auto& orphan : orphans) {
        auto orphan_pkg = db_.get_package(orphan);
        if (orphan_pkg) {
            colors::print_substatus("Removing orphan dependency: " + orphan);
            remove_package_files(*orphan_pkg);
            db_.remove_package(orphan);
        }
    }

    colors::print_success(pkg_name + " removed successfully");
    return true;
}

// ─── Remove Multiple ───────────────────────────────────────────────────────

bool Transaction::remove_multiple(const std::vector<std::string>& packages, bool recursive) {
    bool all_success = true;
    for (const auto& pkg : packages) {
        if (!remove(pkg, recursive)) {
            all_success = false;
        }
    }
    return all_success;
}

// ─── Remove All (--nuke) ───────────────────────────────────────────────────

bool Transaction::remove_all() {
    auto installed = db_.get_all_packages();
    if (installed.empty()) {
        colors::print_warning("No packages are currently installed.");
        return true;
    }

    colors::print_action("Uninstalling " + std::to_string(installed.size()) + " packages...");
    
    // Reverse dependency/install order can help but we are nuking anyway.
    bool all_success = true;
    for (const auto& pkg : installed) {
        colors::print_status("Removing " + pkg.name + "...");
        if (!remove_package_files(pkg)) {
            colors::print_warning("Could not completely remove all files for " + pkg.name);
            all_success = false;
        }
        db_.remove_package(pkg.name);
    }
    
    colors::print_action("Cleaning cache and build artifacts...");
    try {
        if (fs::exists(get_cache_dir())) {
            fs::remove_all(get_cache_dir());
            fs::create_directories(get_cache_dir());
        }
    } catch (const std::exception& e) {
        colors::print_warning("Failed to clear cache directory: " + std::string(e.what()));
    }

    if (all_success) {
        colors::print_success("System entirely cleaned. All packages and caches removed.");
    } else {
        colors::print_warning("System cleaned with some warnings.");
    }
    return true;
}

// ─── Refresh Databases ─────────────────────────────────────────────────────

bool Transaction::refresh_databases() {
    HomebrewBackend brew;
    bool success = brew.refresh_formula_cache();
    brew.refresh_cask_cache(); // Best effort
    return success;
}

// ─── Upgrade All ────────────────────────────────────────────────────────────

bool Transaction::upgrade_all() {
    colors::print_action("Starting full system upgrade...");

    // Refresh databases first
    if (!refresh_databases()) {
        colors::print_error("Failed to synchronize package databases");
        return false;
    }

    // Check each installed package for updates
    auto installed = db_.get_all_packages();
    std::vector<Package> to_upgrade;

    HomebrewBackend brew;

    for (const auto& pkg : installed) {
        if (pkg.source == PackageSource::HOMEBREW) {
            auto remote = brew.get_info(pkg.name);
            if (remote && remote->version != pkg.version) {
                to_upgrade.push_back(*remote);
                std::cout << colors::BOLD_WHITE << pkg.name 
                          << colors::RESET << " " << pkg.version 
                          << " -> " << colors::BOLD_CYAN << remote->version 
                          << colors::RESET << std::endl;
            }
        } else if (pkg.source == PackageSource::AUR) {
            // Check AUR for updates
            AURBackend aur;
            auto remote = aur.get_info(pkg.name);
            
            // For -git packages, remote->version is the last time the PKGBUILD was updated,
            // which might strictly equal pkg.version even if the upstream git repo has new commits.
            // But this is the standard way pacman/yay checks for updates before doing a full git pull.
            if (remote && remote->version != pkg.version) {
                to_upgrade.push_back(*remote);
                std::cout << colors::BOLD_WHITE << pkg.name 
                          << colors::RESET << " " << pkg.version 
                          << " -> " << colors::BOLD_MAGENTA << remote->version 
                          << colors::RESET << std::endl;
            }
        }
    }

    if (to_upgrade.empty()) {
        colors::print_success("there is nothing to do");
        return true;
    }

    // Calculate total size
    size_t total_size = 0;
    for (const auto& p : to_upgrade) {
        total_size += p.download_size;
    }

    // Confirm
    if (!confirm_transaction(TransactionType::UPGRADE, to_upgrade, total_size)) {
        std::cout << "Transaction cancelled." << std::endl;
        return false;
    }

    // Upgrade each package
    for (const auto& pkg : to_upgrade) {
        colors::print_status("Upgrading " + pkg.name + " to " + pkg.version + "...");
        if (!download_and_install(pkg, "explicit")) {
            colors::print_error("Failed to upgrade " + pkg.name);
        }
    }

    colors::print_success("System upgrade complete");
    return true;
}

} // namespace macman
