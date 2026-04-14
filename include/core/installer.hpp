// installer.hpp — Atomic Installation Orchestrator
// Handles downloading, checksumming, staging, fixing RPATHs,
// and atomically publishing the package to its final location.


#pragma once

#include "package.hpp"
#include "database.hpp"
#include <string>
#include <vector>

namespace macman {

class Installer {
public:
    Installer(Database& db);
    ~Installer() = default;

    // Checks SHA256 integrity after downloading payload
    bool verify_checksum(const std::string& file_path, const std::string& expected_sha256) const;

    // Patches compiled Mach-O binaries in macOS to have valid RPATHs
    void fix_macho_rpaths(const std::string& deploy_dir) const;

    // Installs a fully resolved package (handles stages & atomic commit)
    bool install_package(const Package& pkg, const std::string& reason);

    // Atomic move wrapper
    bool atomic_commit(const std::string& stage_dir, const std::string& final_dir) const;

private:
    Database& db_;
};

} // namespace macman
