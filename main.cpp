// Arda Yiğit - Hazani
// main.cpp — Macman Entry Point
// Minimal entry point that initializes the system, parses arguments,
// routes to the correct operation handler, and exits cleanly.


#include "macman.hpp"
#include "cli/argument_parser.hpp"
#include "cli/colors.hpp"
#include "core/config.hpp"
#include "core/database.hpp"
#include "core/transaction.hpp"
#include "core/logger.hpp"
#include "core/self_healing.hpp"
#include "core/i18n.hpp"
#include "backend/homebrew_backend.hpp"
#include "backend/aur_backend.hpp"
#include "net/http_client.hpp"
#include <iostream>
#include <csignal>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <future>

using namespace macman;
namespace fs = std::filesystem;

// --- Signal Handler (Ctrl+C Cleanup) ---

static void signal_handler(int signum) {
    std::cout << std::endl;
    colors::print_warning("Interrupted -- cleaning up...");
    http_global_cleanup();
    std::exit(128 + signum);
}

// --- Display Package Info (for -Si / -Qi) ---

static void display_package_info(const Package& pkg, bool installed = false) {
    auto field = [](const std::string& label, const std::string& value) {
        std::cout << colors::BOLD_WHITE << std::left << std::setw(20) << label 
                  << colors::RESET << ": " << value << std::endl;
    };

    std::string src_str;
    switch (pkg.source) {
        case PackageSource::HOMEBREW: src_str = "MacOS Native (Homebrew)"; break;
        case PackageSource::AUR:     src_str = "Arch Linux AUR"; break;
        case PackageSource::LOCAL:   src_str = "Local"; break;
    }

    field("Repository", src_str);
    field("Name", pkg.name);
    field("Version", pkg.version);
    field("Description", pkg.description);
    field("Homepage", pkg.homepage);
    
    if (!pkg.dependencies.empty()) {
        std::string deps;
        for (size_t i = 0; i < pkg.dependencies.size(); i++) {
            if (i > 0) deps += "  ";
            deps += pkg.dependencies[i];
        }
        field("Depends On", deps);
    } else {
        field("Depends On", "None");
    }
    
    if (pkg.installed_size > 0) {
        field("Installed Size", pkg.format_size(pkg.installed_size));
    }
    if (pkg.download_size > 0) {
        field("Download Size", pkg.format_size(pkg.download_size));
    }
    
    if (installed) {
        field("Install Date", pkg.install_date);
        field("Install Reason", pkg.install_reason);
        field("Files", std::to_string(pkg.installed_files.size()) + " files");
    }

    std::cout << std::endl;
}

// --- Main Function ---

int main(int argc, char* argv[]) {
    // Register signal handler
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Initialize libcurl globally
    http_global_init();

    // Load configuration
    Config::instance().load();

    // Initialize logger
    Logger::instance().init(Config::instance().get_log_file());
    Logger::instance().info("Macman session started");

    // Initialize i18n
    I18n::instance().set_language(Language::TR);

    // Parse arguments
    ArgumentParser parser;
    ParsedArgs args = parser.parse(argc, argv);

    // Load local database
    Database db;
    db.ensure_directories();
    db.load();

    // Lock database for mutation operations
    bool needs_lock = (args.operation == Operation::SYNC_INSTALL || 
                       args.operation == Operation::SYNC_UPGRADE ||
                       args.operation == Operation::SYNC_REFRESH ||
                       args.operation == Operation::REMOVE ||
                       args.operation == Operation::REMOVE_RECURSIVE ||
                       args.operation == Operation::NUKE_ALL);
    
    if (needs_lock && !db.lock()) {
        http_global_cleanup();
        return 1;
    }

    int exit_code = 0;

    // --- Route to Operation Handler ---

    switch (args.operation) {

    // --- Doctor ---
    case Operation::DOCTOR: {
        SelfHealingEngine engine("");
        if (!engine.run_doctor()) exit_code = 1;
        break;
    }

    // --- Version ---
    case Operation::VERSION:
        ArgumentParser::print_version();
        break;

    // --- Help ---
    case Operation::HELP:
        ArgumentParser::print_help();
        break;

    // --- Sync Install (-S <pkg>) ---
    case Operation::SYNC_INSTALL: {
        if (geteuid() != 0) {
            colors::print_error("you cannot perform this operation unless you are root.");
            exit_code = 1;
            break;
        }
        if (args.targets.empty()) {
            colors::print_error("no targets specified (use -h for help)");
            exit_code = 1;
            break;
        }
        Transaction tx(db);
        tx.set_no_confirm(args.no_confirm);
        
        if (!args.no_confirm) {
            // Confirmation happens inside install_multiple, which calls confirm_transaction
        } else {
            // If noconfirm is set, redirect stdin early via dup2
            int fd = open("/dev/null", O_RDONLY);
            if (fd != -1) {
                dup2(fd, STDIN_FILENO);
                close(fd);
            }
        }

        if (!tx.install_multiple(args.targets)) {
            exit_code = 1;
        }
        break;
    }

    // --- Sync Search (-Ss <query>) ---
    case Operation::SYNC_SEARCH: {
        if (args.targets.empty()) {
            colors::print_error("no search term specified");
            exit_code = 1;
            break;
        }
        
        HomebrewBackend brew;
        if (!brew.is_cache_fresh()) {
            brew.refresh_formula_cache();
        }

        for (const auto& query : args.targets) {
            // Launch Homebrew search in background
            auto brew_future = std::async(std::launch::async, [&brew, &query]() {
                return brew.search(query);
            });
            
            // Launch AUR search in background
            auto aur_future = std::async(std::launch::async, [&query]() {
                AURBackend aur;
                return aur.search(query);
            });

            // Wait for both results concurrently
            auto results = brew_future.get();
            auto aur_results = aur_future.get();
            
            results.insert(results.end(), aur_results.begin(), aur_results.end());

            if (results.empty()) {
                colors::print_error("no results found for '" + query + "'");
                exit_code = 1;
            } else {
                for (const auto& pkg : results) {
                    // Mark installed packages
                    if (db.is_installed(pkg.name)) {
                        std::cout << pkg.summary_line() 
                                  << " " << colors::BOLD_CYAN << "[installed]" 
                                  << colors::RESET << std::endl;
                    } else {
                        std::cout << pkg.summary_line() << std::endl;
                    }
                }
            }
        }
        break;
    }

    // --- Sync Info (-Si <pkg>) ---
    case Operation::SYNC_INFO: {
        if (args.targets.empty()) {
            colors::print_error("no targets specified");
            exit_code = 1;
            break;
        }
        HomebrewBackend brew;
        AURBackend aur;

        for (const auto& name : args.targets) {
            auto pkg = brew.get_info_remote(name);
            if (!pkg) {
                pkg = aur.get_info(name);
            }
            if (pkg) {
                display_package_info(*pkg);
            } else {
                colors::print_error("package '" + name + "' was not found");
                exit_code = 1;
            }
        }
        break;
    }

    // --- Sync Refresh (-Sy) ---
    case Operation::SYNC_REFRESH: {
        Transaction tx(db);
        if (!tx.refresh_databases()) {
            exit_code = 1;
        }
        break;
    }

    // --- System Upgrade (-Syu) ---
    case Operation::SYNC_UPGRADE: {
        Transaction tx(db);
        tx.set_no_confirm(args.no_confirm);
        if (!tx.upgrade_all()) {
            exit_code = 1;
        }
        break;
    }

    // --- Sync Upgradable (-Qu) ---
    case Operation::SYNC_UPGRADABLE: {
        Transaction tx(db);
        if (!tx.list_upgradable()) {
            exit_code = 1;
        }
        break;
    }

    // --- Sync Clean (-Sc) ---
    case Operation::SYNC_CLEAN: {
        if (geteuid() != 0) {
            colors::print_error("you cannot perform this operation unless you are root.");
            exit_code = 1;
            break;
        }
        Transaction tx(db);
        if (!tx.clean_cache()) {
            exit_code = 1;
        }
        break;
    }

    // --- Remove (-R <pkg>) ---
    case Operation::REMOVE: {
        if (geteuid() != 0) {
            colors::print_error("you cannot perform this operation unless you are root.");
            exit_code = 1;
            break;
        }
        if (args.targets.empty()) {
            colors::print_error("no targets specified (use -h for help)");
            exit_code = 1;
            break;
        }
        Transaction tx(db);
        tx.set_no_confirm(args.no_confirm);
        if (!tx.remove_multiple(args.targets, false)) {
            exit_code = 1;
        }
        break;
    }

    // --- Remove Recursive (-Rs <pkg>) ---
    case Operation::REMOVE_RECURSIVE: {
        if (geteuid() != 0) {
            colors::print_error("you cannot perform this operation unless you are root.");
            exit_code = 1;
            break;
        }
        if (args.targets.empty()) {
            colors::print_error("no targets specified (use -h for help)");
            exit_code = 1;
            break;
        }
        Transaction tx(db);
        tx.set_no_confirm(args.no_confirm);
        if (!tx.remove_multiple(args.targets, true)) {
            exit_code = 1;
        }
        break;
    }

    // --- Nuke All (--nuke) ---
    case Operation::NUKE_ALL: {
        if (geteuid() != 0) {
            colors::print_error("you cannot perform this operation unless you are root. Nuke requires sudo.");
            exit_code = 1;
            break;
        }
        
        std::cout << std::endl << NUKE_ART << std::endl;
        colors::print_warning("WARNING: This will completely wipe all installed packages and clean the cache!");
        if (!args.no_confirm) {
            std::cout << ":: Are you sure you want to proceed? [y/N] ";
            std::string answer;
            std::getline(std::cin, answer);
            if (answer != "y" && answer != "Y" && answer != "yes") {
                std::cout << "Operation cancelled." << std::endl;
                break;
            }
        }
        
        Transaction tx(db);
        tx.set_no_confirm(true); // Don't ask per package
        if (!tx.remove_all()) {
            exit_code = 1;
        }
        break;
    }

    // --- Query List (-Q) ---
    case Operation::QUERY_LIST: {
        auto packages = db.get_all_packages();
        if (packages.empty()) {
            colors::print_substatus("No packages installed.");
        } else {
            for (const auto& pkg : packages) {
                std::cout << colors::BOLD_WHITE << pkg.name << " "
                          << colors::BOLD_GREEN << pkg.version 
                          << colors::RESET << std::endl;
            }
            std::cout << std::endl << "Total installed packages: " 
                      << packages.size() << std::endl;
        }
        break;
    }

    // --- Query Info (-Qi <pkg>) ---
    case Operation::QUERY_INFO: {
        if (args.targets.empty()) {
            colors::print_error("no targets specified");
            exit_code = 1;
            break;
        }
        for (const auto& name : args.targets) {
            auto pkg = db.get_package(name);
            if (pkg) {
                display_package_info(*pkg, true);
            } else {
                colors::print_error("package '" + name + "' was not found in local database");
                exit_code = 1;
            }
        }
        break;
    }

    // --- Query Files (-Ql <pkg>) ---
    case Operation::QUERY_FILES: {
        if (args.targets.empty()) {
            colors::print_error("no targets specified");
            exit_code = 1;
            break;
        }
        for (const auto& name : args.targets) {
            auto files = db.get_files(name);
            if (files.empty()) {
                colors::print_error("package '" + name + "' was not found or has no files");
                exit_code = 1;
            } else {
                for (const auto& f : files) {
                    std::cout << name << " " << f << std::endl;
                }
            }
        }
        break;
    }

    // --- Query Owner (-Qo <file>) ---
    case Operation::QUERY_OWNS: {
        if (args.targets.empty()) {
            colors::print_error("no file specified");
            exit_code = 1;
            break;
        }
        for (const auto& filepath : args.targets) {
            std::string owner = db.find_owner(filepath);
            if (owner.empty()) {
                colors::print_error(filepath + " is not owned by any package");
                exit_code = 1;
            } else {
                std::cout << filepath << " is owned by " 
                          << colors::BOLD_WHITE << owner << colors::RESET << std::endl;
            }
        }
        break;
    }

    // --- Query Check (-Qk <pkg>) ---
    case Operation::QUERY_CHECK: {
        if (args.targets.empty()) {
            colors::print_error("no targets specified");
            exit_code = 1;
            break;
        }
        for (const auto& name : args.targets) {
            auto pkg = db.get_package(name);
            if (!pkg) {
                colors::print_error("package '" + name + "' not found");
                exit_code = 1;
                continue;
            }

            colors::print_substatus("Checking integrity of " + name + "...");
            int missing = 0;
            for (const auto& f : pkg->installed_files) {
                if (!fs::exists(f)) {
                    std::cout << colors::BOLD_RED << "missing " << colors::RESET << f << std::endl;
                    missing++;
                }
            }

            if (missing == 0) {
                colors::print_success(name + ": all " + std::to_string(pkg->installed_files.size()) + " files present");
            } else {
                colors::print_warning(name + ": " + std::to_string(missing) + " files missing!");
                exit_code = 1;
            }
        }
        break;
    }

    // --- Query Orphans (-Qt) ---
    case Operation::QUERY_ORPHANS: {
        Resolver res(db);
        auto orphans = res.list_orphans();
        if (orphans.empty()) {
            std::cout << "No orphan packages found." << std::endl;
        } else {
            for (const auto& o : orphans) {
                std::cout << colors::BOLD_WHITE << o << colors::RESET << std::endl;
            }
        }
        break;
    }

    // --- Dependency Tree (-T) ---
    case Operation::DEPTREE: {
        if (args.targets.empty()) {
            colors::print_error("no targets specified");
            exit_code = 1;
            break;
        }
        Resolver res(db);
        for (const auto& name : args.targets) {
            res.print_dependency_tree(name);
        }
        break;
    }

    // --- No Operation ---
    case Operation::NONE:
        ArgumentParser::print_usage();
        exit_code = 1;
        break;
    }

    // Cleanup
    http_global_cleanup();
    return exit_code;
}
