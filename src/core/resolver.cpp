// Arda Yiğit - Hazani
// resolver.cpp [V1.2.0 Patch]

#include "core/resolver.hpp"
#include "macman.hpp"
#include "backend/homebrew_backend.hpp"
#include "backend/aur_backend.hpp"
#include "cli/colors.hpp"
#include <iostream>
#include <algorithm>
#include <set>

namespace macman {

Resolver::Resolver(Database& db) : db_(db), brew_() {}

// Extract "sqlite" from "sqlite>=3.45.0"
std::string Resolver::strip_constraint(const std::string& raw_dep) const {
    size_t pos = raw_dep.find_first_of("<>=");
    if (pos != std::string::npos) {
        return raw_dep.substr(0, pos);
    }
    return raw_dep;
}

Package Resolver::resolve_package(const std::string& raw_name) {
    std::string name = strip_constraint(raw_name);

    if (name == "glibc" || name == "linux-headers" || name == "base" || name == "gcc-libs") {
        Package dummy;
        dummy.name = name;
        dummy.version = "macOS-system-stub";
        dummy.source = PackageSource::LOCAL;
        return dummy;
    }

    if (name == "python" || name == "python3") name = "python@3.12";
    if (name == "nodejs" || name == "node") name = "node";

    auto pkg = brew_.get_info(name);
    // If found locally but missing URL, we might need to check remote
    if (pkg && pkg->source == PackageSource::HOMEBREW && pkg->url.empty()) {
        // Fall through to remote check
    } else if (pkg) {
        return *pkg;
    }

    auto brew_future = std::async(std::launch::async, [this, name]() {
        return brew_.get_info_remote(name);
    });

    auto aur_future = std::async(std::launch::async, [name]() {
        AURBackend aur;
        return aur.get_info(name);
    });

    auto brew_pkg_remote = brew_future.get();
    if (brew_pkg_remote) {
        return *brew_pkg_remote;
    }

    auto aur_pkg = aur_future.get();
    if (aur_pkg) {
        return *aur_pkg;
    }

    // Not found
    Package empty;
    empty.name = name;
    return empty;
}

std::vector<Package> Resolver::resolve_all_concurrently(const std::vector<std::string>& targets) {
    std::set<std::string> visited;
    std::vector<std::string> queue;
    
    for (const auto& t : targets) {
        std::string clean = strip_constraint(t);
        if (visited.insert(clean).second) {
            queue.push_back(clean);
        }
    }

    std::vector<Package> to_install;
    bool all_found = true;

    while (!queue.empty()) {
        std::vector<std::future<Package>> futures;
        for (const auto& pkg_name : queue) {
            futures.push_back(std::async(std::launch::async, [this, pkg_name]() {
                return resolve_package(pkg_name);
            }));
        }

        std::vector<std::string> next_queue;
        for (size_t i = 0; i < futures.size(); ++i) {
            Package pkg = futures[i].get();
            if (pkg.version.empty()) {
                colors::print_error("target not found: " + queue[i]);
                all_found = false;
            } else {
                to_install.push_back(pkg);
                
                for (const auto& dep : pkg.dependencies) {
                    std::string clean_dep = strip_constraint(dep);
                    // Skip if already installed
                    if (db_.is_installed(clean_dep)) continue;
                    
                    if (visited.insert(clean_dep).second) {
                        next_queue.push_back(clean_dep);
                    }
                }
            }
        }
        queue = std::move(next_queue);
    }

    if (!all_found) return {};

    // Filter out already installed packages unless they were explicitly requested
    std::vector<Package> final_install_list;
    // Reverse the list so dependencies (which were added later to the queue) come FIRST
    std::reverse(to_install.begin(), to_install.end());
    
    std::set<std::string> added;
    for (const auto& p : to_install) {
        if (added.count(p.name)) continue;
        bool is_target = std::find(targets.begin(), targets.end(), p.name) != targets.end();
        if (!db_.is_installed(p.name) || is_target) {
            final_install_list.push_back(p);
            added.insert(p.name);
        }
    }

    // Concurrently fetch download sizes for packages that have URLs but size 0
    std::vector<std::future<void>> size_futures;
    for (auto& p : final_install_list) {
        if (p.download_size == 0) {
            if (p.source == PackageSource::HOMEBREW && !p.url.empty()) {
                size_futures.push_back(std::async(std::launch::async, [&p]() {
                    HttpClient http;
                    p.download_size = http.get_file_size(p.url);
                }));
            } else if (p.source == PackageSource::AUR) {
                size_futures.push_back(std::async(std::launch::async, [&p]() {
                    HttpClient http;
                    std::string snapshot_url = std::string(AUR_PACKAGE_BASE) + p.name + ".tar.gz";
                    p.download_size = http.get_file_size(snapshot_url);
                }));
            }
        }
    }
    for (auto& f : size_futures) {
        f.get();
    }

    return final_install_list;
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
