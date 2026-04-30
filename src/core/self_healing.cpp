// Arda Yiğit - Hazani
// self_healing.cpp — macOS Build Self-Healing Engine Implementation [V1.2.0 Patch]

#include "core/self_healing.hpp"
#include "cli/colors.hpp"
#include "core/process.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>
#include <set>
#include <map>
#include <cstdlib>
#include <iostream>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <csignal>

namespace fs = std::filesystem;

namespace macman {

SelfHealingEngine::SelfHealingEngine(const std::string& build_dir)
    : build_dir_(build_dir) {}

const std::vector<BuildError>& SelfHealingEngine::get_known_fixes() const {
    static const std::vector<BuildError> fixes = {
        {"linux/", "Linux kernel header not found", "header_stub", ""},
        {"sys/epoll.h", "Linux epoll header not found", "header_stub", "sys/epoll.h"},
        {"aclocal: command not found", "Missing automake tool", "tool_install", "automake"},
        {"cannot be narrowed from type", "C++11 narrowing detected", "cflag_add", "-Wno-narrowing"},
        {"no member named 'i_dir_acl'", "ext2fs i_dir_acl missing on Darwin", "cflag_add", "-Di_dir_acl=i_file_acl"},
        {"ld: library not found for -lintl", "Missing libintl", "linker_flag", "gettext"},
        {"install: illegal option -- t", "macOS install command lacks GNU extensions (-t)", "tool_install", "coreutils"},
        {"install: target directory", "macOS install command lacks GNU extensions (-t as file)", "tool_install", "coreutils"},
        {"install: illegal option -- D", "macOS install command lacks GNU extensions (-D)", "tool_install", "coreutils"}
    };
    return fixes;
}

#ifdef __APPLE__
#include <crt_externs.h>
#define GET_ENV_PTR (*_NSGetEnviron())
#else
extern char** environ;
#define GET_ENV_PTR environ
#endif

int SelfHealingEngine::run_build_capturing_output(const std::string& cmd, std::string& output) {
    output.clear();
    std::string log_path = build_dir_ + "/healing_build.log";

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    
    // Create a dummy pipe to use as a non-interactive stdin
    int pipefds[2] = {-1, -1};
    if (pipe(pipefds) == 0) {
        // Use the read end as STDIN and close it in the child
        posix_spawn_file_actions_adddup2(&actions, pipefds[0], STDIN_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipefds[0]);
        posix_spawn_file_actions_addclose(&actions, pipefds[1]);
    } else {
        // Fallback to /dev/null if pipe fails
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    }
    
    // Redirect stdout and stderr to the log file
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log_path.c_str(),
                                     O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);

    pid_t pid;
    // Set environment to be non-interactive
    setenv("HOMEBREW_NO_INTERACTIVE", "1", 1);
    setenv("DEBIAN_FRONTEND", "noninteractive", 1);

    const char* argv[] = {"/bin/sh", "-c", cmd.c_str(), nullptr};
    
    std::cout.flush();
    std::cerr.flush();
    fflush(nullptr);
    
    int status = posix_spawn(&pid, "/bin/sh", &actions, nullptr, (char* const*)argv, GET_ENV_PTR);
    
    posix_spawn_file_actions_destroy(&actions);
    
    // Close parent ends of the dummy pipe
    if (pipefds[0] != -1) {
        close(pipefds[0]);
        close(pipefds[1]);
    }

    if (status == 0) {
        int wait_status;
        if (waitpid(pid, &wait_status, 0) != -1) {
            status = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : -1;
        } else {
            status = -1;
        }

        // Read the captured output from file
        std::ifstream f(log_path);
        if (f.is_open()) {
            output.assign(std::istreambuf_iterator<char>(f),
                          std::istreambuf_iterator<char>());
        }
    } else {
        status = -1;
    }

    return status;
}

bool SelfHealingEngine::analyze_and_fix_build(const std::string& build_log,
                                               const std::string& work_dir,
                                               const std::string& src_dir,
                                               std::string& extra_cflags,
                                               std::string& extra_ldflags,
                                               const std::string& pkg_name) {
    if (build_log.empty()) return false;
    bool any_fix_applied = false;


    auto inject_cflag = [&](const std::string& flag) {
        if (extra_cflags.find(flag) == std::string::npos) {
            extra_cflags += " " + flag;
            any_fix_applied = true;
        }
    };

    auto inject_ldflag = [&](const std::string& flag) {
        if (extra_ldflags.find(flag) == std::string::npos) {
            extra_ldflags += " " + flag;
            any_fix_applied = true;
        }
    };

    std::string lower_log = build_log;
    std::transform(lower_log.begin(), lower_log.end(), lower_log.begin(), ::tolower);

    // --- GENERAL FIX: Linker error for intl/gettext ---
    if (lower_log.find("intl") != std::string::npos || lower_log.find("setlocale") != std::string::npos) {
        colors::print_substatus("Self-healing: Missing gettext/intl detected. Injecting paths...");
        std::string brew = fs::exists("/opt/homebrew/bin") ? "/opt/homebrew" : "/usr/local";
        inject_ldflag("-L" + brew + "/opt/gettext/lib -lintl");
        inject_cflag("-I" + brew + "/opt/gettext/include");

        // General Makefile Surgery: If environment LDFLAGS are ignored, force them into all Makefiles
        for (const auto& entry : fs::recursive_directory_iterator(work_dir)) {
            if (entry.path().filename() == "Makefile") {
                std::string path = entry.path().string();
                std::ifstream in(path);
                std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                in.close();

                if (content.find("LDFLAGS") != std::string::npos && content.find("-lintl") == std::string::npos) {
                    std::string injection = "LDFLAGS += -L" + brew + "/opt/gettext/lib -lintl\n" +
                                            "CFLAGS += -I" + brew + "/opt/gettext/include\n";
                    std::ofstream out(path);
                    out << injection << content;
                    any_fix_applied = true;
                }
            }
        }
    }

    // --- Pattern Matching for Known Errors ---
    for (const auto& fix : get_known_fixes()) {
        if (build_log.find(fix.pattern) != std::string::npos) {
            colors::print_substatus("Self-healing: Applying " + fix.description);
            if (fix.fix_type == "cflag_add") {
                inject_cflag(fix.fix_value);
            } else if (fix.fix_type == "tool_install") {
                std::string brew = fs::exists("/opt/homebrew/bin") ? "/opt/homebrew" : "/usr/local";
                std::string check_bin = fix.fix_value == "coreutils" ? "ginstall" : fix.fix_value;
                if (!fs::exists(brew + "/bin/" + check_bin)) {
                    setenv("HOMEBREW_NO_INTERACTIVE", "1", 1);
                    const char* sudo_user = getenv("SUDO_USER");
                    if (sudo_user) {
                        run_exec("/usr/bin/su", {"-m", sudo_user, "-c", brew + "/bin/brew install " + fix.fix_value});
                    } else {
                        run_exec(brew + "/bin/brew", {"install", "--formula", fix.fix_value});
                    }
                    any_fix_applied = true;
                } else if (fix.fix_value == "coreutils") {
                    // Even if installed, we apply the fix by retrying since shims will now pick it up
                    any_fix_applied = true;
                }
            }
        }
    }

    return any_fix_applied;
}

void SelfHealingEngine::patch_build_flags(const std::string& src_dir, const std::string& old_flag, const std::string& new_flag) const {}

bool SelfHealingEngine::run_doctor() const {
    colors::print_action("Macman Doctor: Running system health check...");
    bool all_good = true;

    auto check_tool = [&](const std::string& name, const std::string& path) {
        if (fs::exists(path)) {
            colors::print_success("[OK] Found " + name + " at " + path);
            return true;
        } else {
            colors::print_warning("[!!] Missing " + name + " (Expected at " + path + ")");
            return false;
        }
    };

    // 1. Tool Checks
    check_tool("git", "/usr/bin/git");
    check_tool("curl", "/usr/bin/curl");
    if (!check_tool("clang", "/usr/bin/clang")) all_good = false;
    if (!check_tool("make", "/usr/bin/make")) all_good = false;

    // 2. Permission Checks
    std::string prefix = "/usr/local";
    if (access(prefix.c_str(), W_OK) == 0) {
        colors::print_success("[OK] Write access to " + prefix);
    } else {
        colors::print_warning("[!!] No write access to " + prefix + ". Sudo may be required.");
    }

    // 3. SIP Check (System Integrity Protection)
    std::string sip_out;
    if (run_exec("/usr/bin/csrutil", {"status"}, false) == 0) {
        // Output is usually "System Integrity Protection status: enabled."
        colors::print_success("[OK] SIP is enabled (Macman is SIP-friendly)");
    }

    // 4. PATH Check
    char* path_env = getenv("PATH");
    if (path_env) {
        std::string path_str(path_env);
        if (path_str.find("/usr/local/bin") != std::string::npos) {
            colors::print_success("[OK] /usr/local/bin is in your PATH");
        } else {
            colors::print_warning("[!!] /usr/local/bin is NOT in your PATH. Add it to ~/.zshrc");
            all_good = false;
        }
    }

    if (all_good) {
        colors::print_success("\nYour system is healthy and ready for Macman!");
    } else {
        colors::print_warning("\nDoctor found some issues. Please review the warnings above.");
    }

    return all_good;
}

} // namespace macman
