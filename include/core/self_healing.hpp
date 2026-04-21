// self_healing.hpp — macOS Build Self-Healing Engine [V1.2.0 Patch]
// Captures build output, matches ~20 known Linux→macOS error patterns,
// auto-patches source files and compiler flags, retries builds.

#pragma once

#include <string>
#include <vector>

namespace macman {

struct BuildError {
    std::string pattern;        // Substring to match in build log
    std::string description;    // Human-readable explanation
    std::string fix_type;       // "header_stub", "define", "ldflag_remove", etc.
    std::string fix_value;      // The actual fix content
};

class SelfHealingEngine {
public:
    explicit SelfHealingEngine(const std::string& build_dir);

    int  run_build_capturing_output(const std::string& cmd, std::string& output);
    bool analyze_and_fix_build(const std::string& build_log,
                               const std::string& work_dir,
                               const std::string& src_dir,
                               std::string& env_setup);

private:
    std::string build_dir_;

    std::vector<BuildError> get_known_fixes() const;
};

} // namespace macman
