// Arda Yiğit - Hazani
// self_healing.hpp — macOS Build Self-Healing Engine [V1.2.0 Patch]
// Captures build output, matches ~20 known Linux→macOS error patterns,
// auto-patches source files and compiler flags, retries builds.

#pragma once
#include <string>
#include <vector>

namespace macman {

struct BuildError {
    std::string pattern;
    std::string description;
    std::string fix_type;  // "header_stub", "define", "linker_flag", "ldflag_remove", "tool_install", "cflag_add"
    std::string fix_value;
};

class SelfHealingEngine {
public:
    SelfHealingEngine(const std::string& build_dir);

    // Spawns cmd via /bin/sh, redirects both stdout+stderr to a log file,
    // reads it back into `output`, and returns the exit code.
    int  run_build_capturing_output(const std::string& cmd, std::string& output);
    
    // Analyzes the build log and applies fixes to the environment or source.
    // Returns true if at least one fix was applied.
    bool analyze_and_fix_build(const std::string& build_log,
                               const std::string& work_dir,
                               const std::string& src_dir,
                               std::string& extra_cflags,
                               std::string& extra_ldflags,
                               const std::string& pkg_name = "");

    // Performs system checks and prints a status report.
    // Checks for: write permissions, required tools (git, curl, clang), SIP, etc.
    bool run_doctor() const;

private:
    std::string build_dir_;

    const std::vector<BuildError>& get_known_fixes() const;
    void patch_build_flags(const std::string& src_dir, const std::string& old_flag, const std::string& new_flag) const;
};

} // namespace macman
