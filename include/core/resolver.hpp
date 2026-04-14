// resolver.hpp — Dependency Resolution & Engine [V1.1.0 Patch]
// Orchestrates package queries, parallelized remote API lookups,
// and resolves deep dependency trees including constraint mapping.


#pragma once

#include "package.hpp"
#include "database.hpp"
#include <string>
#include <vector>
#include <future>

namespace macman {

class Resolver {
public:
    Resolver(Database& db);
    ~Resolver() = default;

    // Concurrently resolves a package by searching caches, Homebrew, and AUR.
    Package resolve_package(const std::string& name) const;
    
    // Builds a flat list of dependencies required for the target package.
    bool resolve_dependencies(const Package& pkg, std::vector<std::string>& resolved_names) const;
    
    // Resolves multiple top-level targets and their dependencies optimally.
    std::vector<Package> resolve_all_concurrently(const std::vector<std::string>& targets) const;

    // Orphan tracking
    std::vector<std::string> find_orphan_deps(const std::string& pkg_name) const;

    // Parse dependency constraints natively (e.g. "sqlite>=3.4.0")
    std::string strip_constraint(const std::string& raw_dep) const;

private:
    Database& db_;
};

} // namespace macman
