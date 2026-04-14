// resolver.cpp

#include "core/resolver.hpp"
#include "backend/homebrew_backend.hpp"
#include "backend/aur_backend.hpp"
#include "cli/colors.hpp"
#include <iostream>
#include <algorithm>

namespace macman {

Resolver::Resolver(Database& db) : db_(db) {}

// Extract "sqlite" from "sqlite>=3.45.0"
std::string Resolver::strip_constraint(const std::string& raw_dep) const {
    size_t pos = raw_dep.find_first_of("<>=");
    if (pos != std::string::npos) {
        return raw_dep.substr(0, pos);
    }
    return raw_dep;
}

Package Resolver::resolve_package(const std::string& raw_name) const {
    std::string name = strip_constraint(raw_name);

    // Common AUR to Homebrew dependency aliases
    if (name == "python" || name == "python3") name = "python@3.12";
    if (name == "nodejs" || name == "node") name = "node";

    // Try Homebrew cache first (O(1) local hash map)
    HomebrewBackend brew;
    auto pkg = brew.get_info(name);
    if (pkg) return *pkg;

    // Fetch from remotes concurrently
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

bool Resolver::resolve_dependencies(const Package& pkg, std::vector<std::string>& resolved) const {
    for (const auto& dep : pkg.dependencies) {
        std::string clean_dep = strip_constraint(dep);

        // Skip if already installed or in process
        if (db_.is_installed(clean_dep)) continue;
        if (std::find(resolved.begin(), resolved.end(), clean_dep) != resolved.end()) continue;

        resolved.push_back(clean_dep);
        
        Package dep_pkg = resolve_package(clean_dep);
        if (!dep_pkg.version.empty()) {
            resolve_dependencies(dep_pkg, resolved);
        }
    }
    return true;
}

std::vector<Package> Resolver::resolve_all_concurrently(const std::vector<std::string>& targets) const {
    std::vector<std::future<Package>> futures;
    for (const auto& pkg_name : targets) {
        futures.push_back(std::async(std::launch::async, [this, pkg_name]() {
            return resolve_package(pkg_name);
        }));
    }

    std::vector<Package> target_pkgs;
    bool all_found = true;
    for (size_t i = 0; i < futures.size(); ++i) {
        Package pkg = futures[i].get();
        if (pkg.version.empty()) {
            colors::print_error("target not found: " + targets[i]);
            all_found = false;
        } else {
            target_pkgs.push_back(pkg);
        }
    }
    if (!all_found) return {};

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

    // Append targets
    to_install.insert(to_install.end(), target_pkgs.begin(), target_pkgs.end());

    // Deduplicate
    std::vector<Package> unique_to_install;
    for (const auto& p : to_install) {
        bool duplicate = false;
        for (const auto& u : unique_to_install) {
            if (u.name == p.name) { duplicate = true; break; }
        }
        bool is_target = std::find(targets.begin(), targets.end(), p.name) != targets.end();
        if (!duplicate && (!db_.is_installed(p.name) || is_target)) {
            unique_to_install.push_back(p);
        }
    }

    // Concurrently fetch download sizes for packages that have URLs but size 0
    std::vector<std::future<void>> size_futures;
    for (auto& p : unique_to_install) {
        if (p.source == PackageSource::HOMEBREW && p.download_size == 0 && !p.url.empty()) {
            size_futures.push_back(std::async(std::launch::async, [&p]() {
                HttpClient http;
                p.download_size = http.get_file_size(p.url);
            }));
        }
    }
    for (auto& f : size_futures) {
        f.get();
    }

    return unique_to_install;
}

std::vector<std::string> Resolver::find_orphan_deps(const std::string& pkg_name) const {
    std::vector<std::string> orphans;
    auto pkg_opt = db_.get_package(pkg_name);
    if (!pkg_opt) return orphans;

    for (const auto& dep : pkg_opt->dependencies) {
        std::string clean_dep = strip_constraint(dep);
        auto dep_pkg = db_.get_package(clean_dep);
        if (!dep_pkg) continue;
        if (dep_pkg->install_reason != "dependency") continue;

        bool is_needed = false;
        for (const auto& other : db_.get_all_packages()) {
            if (other.name == pkg_name) continue;
            for (const auto& other_dep : other.dependencies) {
                if (strip_constraint(other_dep) == clean_dep) {
                    is_needed = true;
                    break;
                }
            }
            if (is_needed) break;
        }

        if (!is_needed) {
            orphans.push_back(clean_dep);
        }
    }
    return orphans;
}

} // namespace macman
