// Arda Yiğit - Hazani
// installer.hpp — Atomic Installation Orchestrator [V1.2.0 Patch]
// Handles downloading, checksumming, staging, fixing RPATHs,
// and atomically publishing the package to its final location.
// V1.2.0: Self-healing compilation engine — captures make/cmake stderr,
// analyzes clang/gcc errors via regex, auto-patches Darwin incompatibilities,
// and retries the build up to MAX_COMPILE_RETRIES times.


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

    // Links a keg-only package from /opt to global prefix using symlinks
    bool link_to_prefix(const std::string& pkg_dir, std::vector<std::string>& installed_files) const;

    // Calculates and records hashes for all files in pkg_dir
    void record_hashes(const std::string& pkg_dir, std::map<std::string, std::string>& hashes) const;

    // Atomic move wrapper
    bool atomic_commit(const std::string& stage_dir, const std::string& final_dir) const;

    // ── Self-Healing Compilation Engine ─────────────────────────────────────
    // Runs base_cmd under a macOS-safe environment, captures stderr, detects
    // clang/gcc/ld errors via regex, applies Darwin compatibility patches to
    // source files and compiler flags, then retries up to max_retries times.
    bool build_with_healing(const std::string& base_cmd,
                             const std::string& src_dir,
                             int max_retries = MAX_COMPILE_RETRIES) const;

private:
    Database& db_;

    static constexpr int MAX_COMPILE_RETRIES = 8;

    // Spawns cmd via /bin/sh, redirects both stdout+stderr to a log file,
    // reads it back into `output`, and returns the exit code.
    int run_capturing_output(const std::string& cmd, std::string& output) const;

    // Scans build_log with regex, writes fixes to compat_dir/macman_compat.h,
    // patches Makefile/CMakeLists.txt in src_dir, and appends -I/-include to
    // extra_cflags so the next compile attempt picks them up.
    // Returns true if at least one fix was applied.
    bool analyze_and_fix_compile_errors(const std::string& build_log,
                                         const std::string& src_dir,
                                         const std::string& compat_dir,
                                         std::string& extra_cflags) const;

    // Removes or replaces old_flag with new_flag (empty = delete) in every
    // Makefile, CMakeLists.txt, .mk, and configure script under src_dir.
    void patch_build_flags(const std::string& src_dir,
                            const std::string& old_flag,
                            const std::string& new_flag) const;

    // Comments out find_package(PKG REQUIRED) / find_program for `pkg_name`
    // in all CMakeLists.txt files under src_dir (strips REQUIRED keyword).
    void patch_cmake_remove_required(const std::string& src_dir,
                                      const std::string& pkg_name) const;
};

} // namespace macman
