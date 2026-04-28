// Arda Yiğit - Hazani
// remover.cpp [V1.2.0 Patch]

#include "core/remover.hpp"
#include "core/resolver.hpp"
#include "macman.hpp"
#include "cli/colors.hpp"
#include <iostream>
#include <filesystem>

namespace fs = std::filesystem;

namespace macman {

Remover::Remover(Database& db) : db_(db) {}

bool Remover::remove_package(const Package& pkg) const {
    colors::print_status("Removing " + pkg.name + "...");
    bool all_success = true;

    for (const auto& file : pkg.installed_files) {
        // file is already an absolute path (including prefix)
        fs::path p(file);
        try {
            if (fs::exists(p) || fs::is_symlink(p)) {
                if (fs::is_directory(p)) {
                    // For our /opt/<pkg> directories, we want to remove everything inside
                    if (p.string().find("/opt/" + pkg.name) != std::string::npos) {
                        fs::remove_all(p);
                    } else if (fs::is_empty(p)) {
                        fs::remove(p);
                    }
                } else {
                    fs::remove(p);
                }
            }
        } catch (const std::exception& e) {
            all_success = false;
        }
    }

    if (!all_success) {
        colors::print_warning("Some files could not be cleanly removed for " + pkg.name);
    }
    db_.remove_package(pkg.name);
    return true;
}

bool Remover::nuke_system() const {
    auto installed = db_.get_all_packages();
    if (installed.empty()) {
        colors::print_warning("No packages are currently installed.");
        return true;
    }

    colors::print_action("Uninstalling " + std::to_string(installed.size()) + " packages...");
    
    bool all_success = true;
    for (const auto& pkg : installed) {
        if (!remove_package(pkg)) {
            all_success = false;
        }
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

} // namespace macman
