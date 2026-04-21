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
#include <cstdlib>
#include <iostream>
#include <spawn.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace macman {

SelfHealingEngine::SelfHealingEngine(const std::string& build_dir)
    : build_dir_(build_dir) {}

// --- Known Build Error Fixes ---

std::vector<BuildError> SelfHealingEngine::get_known_fixes() const {
    return {
        // Missing Linux-specific headers → create empty stubs
        {"linux/", "Linux kernel header not found",
         "header_stub", ""},
        {"sys/sendfile.h", "Linux sendfile header not found",
         "header_stub", "sys/sendfile.h"},
        {"sys/inotify.h", "Linux inotify header not found",
         "header_stub", "sys/inotify.h"},
        {"sys/signalfd.h", "Linux signalfd header not found",
         "header_stub", "sys/signalfd.h"},
        {"sys/timerfd.h", "Linux timerfd header not found",
         "header_stub", "sys/timerfd.h"},
        {"sys/epoll.h", "Linux epoll header not found",
         "header_stub", "sys/epoll.h"},
        {"sys/prctl.h", "Linux prctl header not found",
         "header_stub", "sys/prctl.h"},
        {"malloc.h", "malloc.h is not used on macOS",
         "header_stub", "malloc.h"},
        {"endian.h", "endian.h not found, stubbing (handled by compat header)",
         "header_stub", "endian.h"},
        {"byteswap.h", "byteswap.h not found, stubbing (handled by compat header)",
         "header_stub", "byteswap.h"},
        {"pty.h", "pty.h not found, stubbing",
         "header_stub", "pty.h"},
        {"syscall.h", "syscall.h not on macOS — redirecting to sys/syscall.h",
         "header_stub", "syscall.h"},
        {"features.h", "glibc features.h not on macOS — stubbing",
         "header_stub", "features.h"},
        {"error.h", "glibc error.h not on macOS — redirecting to err.h",
         "header_stub", "error.h"},

        // _POSIX_C_SOURCE defined without value (e.g. #define _POSIX_C_SOURCE with no number)
        // macOS unistd.h does "_POSIX_C_SOURCE < 200112L" which fails on empty token.
        // Also inject _DARWIN_C_SOURCE so syscall() is exposed by macOS unistd.h.
        {"invalid token at start of a preprocessor expression",
         "_POSIX_C_SOURCE defined without value — injecting POSIX.1-2008 + Darwin extensions",
         "define",
         "#ifndef _DARWIN_C_SOURCE\n#define _DARWIN_C_SOURCE\n#endif\n"
         "#ifndef _POSIX_C_SOURCE\n#define _POSIX_C_SOURCE 200809L\n#endif"},

        // Linux kernel integer types (__u8/__u16/__u32/__u64 etc.) not in macOS headers
        {"unknown type name '__u",
         "Linux kernel integer types missing — adding stdint-based stubs",
         "define",
         "#ifndef __u8\ntypedef unsigned char __u8;\n#endif\n"
         "#ifndef __u16\ntypedef unsigned short __u16;\n#endif\n"
         "#ifndef __u32\ntypedef unsigned int __u32;\n#endif\n"
         "#ifndef __u64\ntypedef unsigned long long __u64;\n#endif\n"
         "#ifndef __s8\ntypedef signed char __s8;\n#endif\n"
         "#ifndef __s16\ntypedef signed short __s16;\n#endif\n"
         "#ifndef __s32\ntypedef signed int __s32;\n#endif\n"
         "#ifndef __s64\ntypedef signed long long __s64;\n#endif"},

        // sched_getscheduler is Linux-specific, not available on macOS
        {"sched_getscheduler",
         "sched_getscheduler not available on macOS — stubbing",
         "define",
         "#include <sys/types.h>\n"
         "#ifndef __MACMAN_SCHED_GETSCHEDULER\n#define __MACMAN_SCHED_GETSCHEDULER\n"
         "static inline int sched_getscheduler(pid_t p){(void)p;return 0;}\n#endif"},

        // Undeclared identifiers → add #define or typedef
        {"MSG_NOSIGNAL", "MSG_NOSIGNAL not available on macOS",
         "define", "#ifndef MSG_NOSIGNAL\n#define MSG_NOSIGNAL 0\n#endif"},
        {"O_TMPFILE", "O_TMPFILE not available on macOS",
         "define", "#ifndef O_TMPFILE\n#define O_TMPFILE 0\n#endif"},
        {"__pid_t", "__pid_t not defined on macOS",
         "define", "#ifndef __pid_t\ntypedef pid_t __pid_t;\n#endif"},
        {"pipe2", "pipe2() not available on macOS",
         "define", "#ifndef pipe2\n#define pipe2(fds, flags) pipe(fds)\n#endif"},
        {"accept4", "accept4() not available on macOS",
         "define", "#ifndef accept4\n#define accept4(fd, addr, len, flags) accept(fd, addr, len)\n#endif"},
        {"getauxval", "getauxval() not available on macOS",
         "define", "#ifndef getauxval\nstatic inline unsigned long getauxval(unsigned long t) { (void)t; return 0; }\n#endif"},
        {"TEMP_FAILURE_RETRY", "TEMP_FAILURE_RETRY not available on macOS",
         "define", "#ifndef TEMP_FAILURE_RETRY\n#define TEMP_FAILURE_RETRY(exp) ({ \\\n    __typeof__(exp) _rc; \\\n    do { \\\n        _rc = (exp); \\\n    } while (_rc == -1 && errno == EINTR); \\\n    _rc; \\\n})\n#endif"},
        {"malloc_usable_size", "malloc_usable_size() not on macOS (use malloc_size)",
         "define", "#ifdef __APPLE__\n#include <malloc/malloc.h>\n#define malloc_usable_size(p) malloc_size(p)\n#endif"},

        // CMake missing tool/executable errors → patch CMakeLists.txt
        {"sphinx-build' not found", "Disabling Sphinx documentation build",
         "cmake_patch", "sphinx"},
        {"sphinx-build: not found", "Disabling Sphinx documentation build",
         "cmake_patch", "sphinx"},
        {"Sphinx' not found", "Disabling Sphinx documentation build",
         "cmake_patch", "sphinx"},
        {"doxygen: not found", "Disabling Doxygen documentation build",
         "cmake_patch", "doxygen"},
        {"Doxygen' not found", "Disabling Doxygen documentation build",
         "cmake_patch", "doxygen"},
        {"Could NOT find ALSA", "Disabling ALSA (Linux-only audio)",
         "cmake_patch", "alsa"},
        {"Could NOT find LibNL", "Disabling libnl (Linux-only networking)",
         "cmake_patch", "libnl"},
        {"Could NOT find Libnl", "Disabling libnl (Linux-only networking)",
         "cmake_patch", "libnl"},
        {"i3ipc library not found", "Disabling i3wm IPC support",
         "cmake_patch", "i3"},
        {"Could NOT find PulseAudio", "Disabling PulseAudio (Linux-only)",
         "cmake_patch", "pulseaudio"},

        // Missing build tools → install via Homebrew
        {"Could NOT find PkgConfig", "Installing pkg-config via Homebrew",
         "tool_install", "pkg-config"},
        {"PKG_CONFIG_EXECUTABLE", "Installing pkg-config via Homebrew",
         "tool_install", "pkg-config"},
        {"Could not find a package configuration file provided by \"Python", "Installing python3 via Homebrew",
         "tool_install", "python3"},
        {"Could NOT find Python", "Installing python3 via Homebrew",
         "tool_install", "python3"},

        // Unsupported compiler checks → patch cmake files
        {"unsupported compiler", "Patching unsupported compiler check for AppleClang",
         "cmake_patch", "unsupported"},
        {"Unsupported compiler", "Patching unsupported compiler check for AppleClang",
         "cmake_patch", "unsupported"},

        // Linker errors → remove Linux-only libs
        {"library not found for -lrt", "librt not needed on macOS",
         "ldflag_remove", "-lrt"},
        {"library not found for -ldl", "libdl not needed on macOS",
         "ldflag_remove", "-ldl"},
        {"library not found for -lpthread", "libpthread built-in on macOS",
         "ldflag_remove", "-lpthread"},
        {"unknown option: --as-needed", "GNU ld option not supported by macOS ld",
         "ldflag_remove", "-Wl,--as-needed"},
        {"unknown option: --no-as-needed", "GNU ld option not supported by macOS ld",
         "ldflag_remove", "-Wl,--no-as-needed"},
        {"unknown option: --no-undefined", "GNU ld option not supported by macOS ld",
         "ldflag_remove", "-Wl,--no-undefined"},

        // Compiler flag errors
        {"-Werror=format-truncation", "GCC-specific warning flag",
         "cflag_remove", "-Werror=format-truncation"},
        {"-Wno-format-truncation", "GCC-specific warning flag",
         "cflag_remove", "-Wno-format-truncation"},

        // pkg-config missing packages → install via Homebrew
        {"Package '" , "Auto-installing missing pkg-config dependency",
         "pkg_not_found", ""},
        {"required packages were not found", "Auto-installing missing pkg-config dependencies",
         "pkg_not_found", ""},

        // C++11 narrowing errors (common in old code)
        {"cannot be narrowed from type", "C++11 narrowing detected — adding -Wno-narrowing",
         "cflag_add", "-Wno-narrowing"},
        {"narrowing conversion", "C++11 narrowing detected — adding -Wno-narrowing",
         "cflag_add", "-Wno-narrowing"},

        // BSD ar empty archive (header-only libs on macOS)
        {"no archive members specified", "Fixing empty static library for macOS BSD ar",
         "empty_archive", ""},
    };
}

// --- Run Build Command and Capture Output ---

#ifdef __APPLE__
#include <crt_externs.h>
#define GET_ENVIRON (*_NSGetEnviron())
#else
extern char **environ;
#define GET_ENVIRON environ
#endif

int SelfHealingEngine::run_build_capturing_output(const std::string& cmd, std::string& output) {
    output.clear();
    std::string log_file = build_dir_ + "/build_output.log";

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);

    pid_t pid;
    const char* argv[] = {"sh", "-c", cmd.c_str(), nullptr};

    int status = posix_spawn(&pid, "/bin/sh", &actions, nullptr, (char* const*)argv, GET_ENVIRON);
    posix_spawn_file_actions_destroy(&actions);

    if (status == 0) {
        int wait_status;
        waitpid(pid, &wait_status, 0);
        status = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : -1;
    } else {
        status = -1;
    }

    try {
        std::ifstream file(log_file);
        if (file.is_open()) {
            output = std::string((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
        }
    } catch (...) {}

    return status;
}

// --- Analyze Build Log and Apply Fixes ---

bool SelfHealingEngine::analyze_and_fix_build(const std::string& build_log,
                                               const std::string& work_dir,
                                               const std::string& src_dir,
                                               std::string& env_setup) {
    auto known_fixes = get_known_fixes();
    bool any_fix_applied = false;

    std::string compat_dir = work_dir + "/macman_compat";
    fs::create_directories(compat_dir);

    std::string compat_header = compat_dir + "/macman_compat.h";

    std::string compat_content;
    if (fs::exists(compat_header)) {
        std::ifstream existing(compat_header);
        compat_content = std::string((std::istreambuf_iterator<char>(existing)),
                                      std::istreambuf_iterator<char>());
    } else {
        compat_content = "#pragma once\n/* macman auto-generated macOS compatibility header */\n\n";
    }

    for (const auto& fix : known_fixes) {
        if (build_log.find(fix.pattern) == std::string::npos) continue;

        colors::print_substatus("Self-healing: " + fix.description);

        if (fix.fix_type == "header_stub") {
            if (!fix.fix_value.empty()) {
                fs::path header_path = fs::path(compat_dir) / fix.fix_value;
                fs::create_directories(header_path.parent_path());
                if (!fs::exists(header_path)) {
                    std::ofstream stub(header_path.string());
                    // Known redirects: some Linux headers have macOS equivalents
                    std::string stub_content;
                    if (fix.fix_value == "syscall.h")  stub_content = "#include <sys/syscall.h>\n";
                    else if (fix.fix_value == "error.h") stub_content = "#include <err.h>\n";
                    else if (fix.fix_value == "features.h") stub_content = "/* glibc features.h not on macOS */\n";
                    if (!stub_content.empty()) {
                        stub << stub_content;
                    } else {
                        stub << "/* macman auto-stub: " << fix.fix_value << " */\n";
                        stub << "/* This header is not available on macOS. */\n";
                    }
                    stub.close();
                    any_fix_applied = true;
                }
            } else {
                std::regex header_re("fatal error: '(linux/[^']+)' file not found");
                std::smatch match;
                if (std::regex_search(build_log, match, header_re)) {
                    std::string header_name = match[1].str();
                    fs::path header_path = fs::path(compat_dir) / header_name;
                    fs::create_directories(header_path.parent_path());
                    if (!fs::exists(header_path)) {
                        std::ofstream stub(header_path.string());
                        // Provide real content for headers whose types/macros are used directly
                        if (header_name == "linux/types.h") {
                            stub <<
                                "#pragma once\n"
                                "#ifndef __u8\ntypedef unsigned char __u8;\n#endif\n"
                                "#ifndef __u16\ntypedef unsigned short __u16;\n#endif\n"
                                "#ifndef __u32\ntypedef unsigned int __u32;\n#endif\n"
                                "#ifndef __u64\ntypedef unsigned long long __u64;\n#endif\n"
                                "#ifndef __s8\ntypedef signed char __s8;\n#endif\n"
                                "#ifndef __s16\ntypedef signed short __s16;\n#endif\n"
                                "#ifndef __s32\ntypedef signed int __s32;\n#endif\n"
                                "#ifndef __s64\ntypedef signed long long __s64;\n#endif\n"
                                "#ifndef __be16\ntypedef unsigned short __be16;\n#endif\n"
                                "#ifndef __be32\ntypedef unsigned int __be32;\n#endif\n"
                                "#ifndef __le16\ntypedef unsigned short __le16;\n#endif\n"
                                "#ifndef __le32\ntypedef unsigned int __le32;\n#endif\n"
                                "#ifndef __le64\ntypedef unsigned long long __le64;\n#endif\n";
                        } else if (header_name == "linux/capability.h") {
                            stub <<
                                "#pragma once\n"
                                "#include <stdint.h>\n"
                                "#ifndef __u32\ntypedef unsigned int __u32;\n#endif\n"
                                "#define _LINUX_CAPABILITY_VERSION_3 0x20080522\n"
                                "#define _LINUX_CAPABILITY_U32S_3 2\n"
                                "#define CAP_NET_ADMIN 12\n"
                                "#define CAP_TO_INDEX(x) ((x)>>5)\n"
                                "#define CAP_TO_MASK(x) (1U<<((x)&31))\n"
                                "struct __user_cap_header_struct{__u32 version;int pid;};\n"
                                "struct __user_cap_data_struct{__u32 effective;__u32 permitted;__u32 inheritable;};\n"
                                "#ifndef SYS_capget\n#define SYS_capget 90\n#endif\n";
                        } else {
                            stub << "/* macman auto-stub: " << header_name << " */\n";
                            stub << "/* This Linux kernel header is not available on macOS. */\n";
                        }
                        stub.close();
                        any_fix_applied = true;
                    }
                }
            }
        }
        else if (fix.fix_type == "define" || fix.fix_type == "typedef") {
            if (compat_content.find(fix.pattern) == std::string::npos) {
                compat_content += "\n" + fix.fix_value + "\n";
                any_fix_applied = true;
            }
        }
        else if (fix.fix_type == "ldflag_remove") {
            std::string flag = fix.fix_value;
            size_t pos;
            while ((pos = env_setup.find(flag)) != std::string::npos) {
                env_setup.erase(pos, flag.length());
                any_fix_applied = true;
            }

            for (const auto& entry : fs::recursive_directory_iterator(src_dir)) {
                if (!entry.is_regular_file()) continue;
                std::string fname = entry.path().filename().string();
                if (fname == "Makefile" || fname == "CMakeLists.txt" ||
                    fname == "configure" || fname.find(".mk") != std::string::npos) {
                    try {
                        std::string content;
                        {
                            std::ifstream f(entry.path());
                            content = std::string((std::istreambuf_iterator<char>(f)),
                                                   std::istreambuf_iterator<char>());
                        }
                        if (content.find(flag) != std::string::npos) {
                            size_t p;
                            while ((p = content.find(flag)) != std::string::npos) {
                                content.erase(p, flag.length());
                            }
                            std::ofstream f(entry.path());
                            f << content;
                            any_fix_applied = true;
                        }
                    } catch (...) {}
                }
            }
        }
        else if (fix.fix_type == "cflag_remove") {
            std::string flag = fix.fix_value;
            size_t pos;
            while ((pos = env_setup.find(flag)) != std::string::npos) {
                env_setup.erase(pos, flag.length());
                any_fix_applied = true;
            }
        }
        else if (fix.fix_type == "cflag_add") {
            std::string flag = fix.fix_value;
            if (env_setup.find(flag) == std::string::npos) {
                auto inject_into_var = [&](const std::string& var) {
                    size_t pos = env_setup.find(var + "=\"");
                    char quote_char = '\"';
                    if (pos == std::string::npos) {
                        pos = env_setup.find(var + "='");
                        quote_char = '\'';
                    }
                    
                    if (pos != std::string::npos) {
                        size_t quote_end = env_setup.find(quote_char, pos + var.length() + 2);
                        if (quote_end != std::string::npos) {
                            env_setup.insert(quote_end, " " + flag);
                        }
                    }
                };
                inject_into_var("CFLAGS");
                inject_into_var("CXXFLAGS");
                any_fix_applied = true;
            }
        }
        else if (fix.fix_type == "cmake_patch") {
            std::string tool = fix.fix_value;

            for (const auto& entry : fs::recursive_directory_iterator(src_dir)) {
                if (!entry.is_regular_file()) continue;
                std::string fname = entry.path().filename().string();
                if (fname != "CMakeLists.txt" && fname.find(".cmake") == std::string::npos) continue;

                try {
                    std::string content;
                    {
                        std::ifstream f(entry.path());
                        content = std::string((std::istreambuf_iterator<char>(f)),
                                               std::istreambuf_iterator<char>());
                    }

                    bool modified = false;
                    std::string lower_tool = tool;
                    std::transform(lower_tool.begin(), lower_tool.end(), lower_tool.begin(), ::tolower);

                    std::istringstream stream(content);
                    std::string line;
                    std::string new_content;

                    while (std::getline(stream, line)) {
                        std::string lower_line = line;
                        std::transform(lower_line.begin(), lower_line.end(), lower_line.begin(), ::tolower);

                        bool should_comment = false;

                        if ((lower_line.find("find_program") != std::string::npos ||
                             lower_line.find("find_package") != std::string::npos) &&
                            lower_line.find(lower_tool) != std::string::npos) {
                            should_comment = true;
                        }

                        if ((lower_tool == "sphinx" || lower_tool == "doxygen") &&
                            lower_line.find("add_subdirectory") != std::string::npos &&
                            lower_line.find("doc") != std::string::npos) {
                            should_comment = true;
                        }

                        if (lower_line.find("fatal_error") != std::string::npos &&
                            lower_line.find(lower_tool) != std::string::npos) {
                            should_comment = true;
                        }

                        if (should_comment) {
                            new_content += "# [macman patched] " + line + "\n";
                            modified = true;
                        } else {
                            if ((lower_line.find("find_package") != std::string::npos) &&
                                lower_line.find(lower_tool) != std::string::npos &&
                                line.find("REQUIRED") != std::string::npos) {
                                size_t rpos = line.find("REQUIRED");
                                line.erase(rpos, 8);
                                modified = true;
                            }
                            new_content += line + "\n";
                        }
                    }

                    if (modified) {
                        std::ofstream f(entry.path());
                        f << new_content;
                        any_fix_applied = true;
                    }
                } catch (...) {}
            }
        }
        else if (fix.fix_type == "tool_install") {
            std::string tool = fix.fix_value;
            std::string brew_prefix = fs::exists("/opt/homebrew/bin") ? "/opt/homebrew" : "/usr/local";
            std::string tool_path = brew_prefix + "/bin/" + tool;

            if (!fs::exists(tool_path)) {
                run_exec(brew_prefix + "/bin/brew", {"install", tool});
            }

            if (env_setup.find("export PATH=") == std::string::npos) {
                env_setup = "export PATH=\"" + brew_prefix + "/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH\" && " + env_setup;
            }

            if (tool == "pkg-config" && fs::exists(brew_prefix + "/bin/pkg-config")) {
                env_setup += " PKG_CONFIG_EXECUTABLE='" + brew_prefix + "/bin/pkg-config' ";
            }

            any_fix_applied = true;
        }
        else if (fix.fix_type == "pkg_not_found") {
            std::string brew_prefix = fs::exists("/opt/homebrew/bin") ? "/opt/homebrew" : "/usr/local";
            const char* sudo_user = std::getenv("SUDO_USER");

            std::vector<std::pair<std::string, std::string>> pkg_to_brew = {
                {"cairo-fc", "cairo"}, {"cairo-ft", "cairo"}, {"cairo-pdf", "cairo"},
                {"cairo-png", "cairo"}, {"cairo-svg", "cairo"}, {"cairo-xlib", "cairo"},
                {"cairo-xcb", "cairo"}, {"cairo", "cairo"},
                {"pangocairo", "pango"}, {"pango", "pango"},
                {"glib-2.0", "glib"}, {"gobject-2.0", "glib"}, {"gio-2.0", "glib"},
                {"jsoncpp", "jsoncpp"}, {"libcurl", "curl"}, {"libuv", "libuv"},
                {"xcb-proto", "xcb-proto"}, {"xcb-util-image", "xcb-util-image"},
                {"xcb-util-wm", "xcb-util-wm"}, {"xcb-util-xrm", "xcb-util-xrm"},
                {"xcb-util-cursor", "xcb-util-cursor"}, {"xcb-util-renderutil", "xcb-util-renderutil"},
                {"xcb-util-keysyms", "xcb-util-keysyms"}, {"xcb-util", "xcb-util"},
                {"xcb-image", "xcb-util-image"}, {"xcb-ewmh", "xcb-util-wm"},
                {"xcb-icccm", "xcb-util-wm"}, {"xcb-xrm", "xcb-util-xrm"},
                {"xcb-cursor", "xcb-util-cursor"}, {"xcb-renderutil", "xcb-util-renderutil"},
                {"xcb-xkb", "libxcb"}, {"xcb-randr", "libxcb"}, {"xcb-composite", "libxcb"},
                {"xcb-shape", "libxcb"}, {"xcb-shm", "libxcb"}, {"xcb-render", "libxcb"},
                {"xcb", "libxcb"}, {"xproto", "xorgproto"},
                {"x11", "libx11"}, {"x11-xcb", "libx11"}, {"xext", "libxext"},
                {"fontconfig", "fontconfig"}, {"freetype2", "freetype"},
                {"harfbuzz", "harfbuzz"}, {"libpng", "libpng"},
                {"pixman-1", "pixman"}, {"zlib", "zlib"}, {"expat", "expat"},
            };

            std::set<std::string> pkgs_to_install;

            std::regex pkg_re("Package '([^']+)' not found");
            auto it = std::sregex_iterator(build_log.begin(), build_log.end(), pkg_re);
            for (; it != std::sregex_iterator(); ++it) {
                pkgs_to_install.insert((*it)[1].str());
            }

            std::regex dash_re("^\\s+-\\s+(\\S+)", std::regex::multiline);
            auto it2 = std::sregex_iterator(build_log.begin(), build_log.end(), dash_re);
            for (; it2 != std::sregex_iterator(); ++it2) {
                pkgs_to_install.insert((*it2)[1].str());
            }

            for (const auto& pkg_name : pkgs_to_install) {
                std::string brew_name = pkg_name;
                bool found_exact = false;
                for (const auto& [pc_name, formula] : pkg_to_brew) {
                    if (pkg_name == pc_name) { brew_name = formula; found_exact = true; break; }
                }
                if (!found_exact) {
                    for (const auto& [pc_name, formula] : pkg_to_brew) {
                        if (pkg_name.find(pc_name) == 0) { brew_name = formula; break; }
                    }
                }

                bool already_available = false;
                for (const auto& opt_entry : fs::directory_iterator(brew_prefix + "/opt")) {
                    std::string pc1 = opt_entry.path().string() + "/lib/pkgconfig/" + pkg_name + ".pc";
                    std::string pc2 = opt_entry.path().string() + "/share/pkgconfig/" + pkg_name + ".pc";
                    if (fs::exists(pc1) || fs::exists(pc2)) { already_available = true; break; }
                }
                if (already_available) continue;

                colors::print_substatus("Self-healing: Installing " + brew_name + " (provides " + pkg_name + ")...");
                std::string brew_bin = brew_prefix + "/bin/brew";
                if (sudo_user) {
                    run_exec("/usr/bin/sudo", {"-u", sudo_user, brew_bin, "install", brew_name});
                } else {
                    run_exec(brew_bin, {"install", brew_name});
                }
                any_fix_applied = true;
            }

            std::string extra_pc_paths;
            for (const auto& entry : fs::directory_iterator(brew_prefix + "/opt")) {
                std::string pc_lib   = entry.path().string() + "/lib/pkgconfig";
                std::string pc_share = entry.path().string() + "/share/pkgconfig";
                if (fs::exists(pc_lib))   extra_pc_paths += pc_lib + ":";
                if (fs::exists(pc_share)) extra_pc_paths += pc_share + ":";
            }
            if (!extra_pc_paths.empty()) {
                size_t pos = env_setup.find("PKG_CONFIG_PATH='");
                if (pos != std::string::npos) {
                    size_t val_start = pos + 17;
                    env_setup.insert(val_start, extra_pc_paths);
                }
                any_fix_applied = true;
            }
        }
        else if (fix.fix_type == "empty_archive") {
            std::regex lib_re("Linking CXX static library ([^\\n]+\\.a)");
            std::smatch lib_match;
            if (std::regex_search(build_log, lib_match, lib_re)) {
                std::string lib_path = lib_match[1].str();
                std::string lib_filename = fs::path(lib_path).filename().string();
                std::string lib_name = lib_filename;
                if (lib_name.find("lib") == 0) lib_name = lib_name.substr(3);
                if (lib_name.size() > 2 && lib_name.substr(lib_name.size() - 2) == ".a") {
                    lib_name = lib_name.substr(0, lib_name.size() - 2);
                }

                for (const auto& entry : fs::recursive_directory_iterator(src_dir)) {
                    if (!entry.is_regular_file()) continue;
                    if (entry.path().filename() != "CMakeLists.txt") continue;

                    std::string content;
                    {
                        std::ifstream f(entry.path());
                        content = std::string((std::istreambuf_iterator<char>(f)),
                                               std::istreambuf_iterator<char>());
                    }

                    if (content.find("add_library(" + lib_name) != std::string::npos) {
                        std::string dummy_path = entry.path().parent_path().string() + "/macman_dummy.cpp";
                        if (!fs::exists(dummy_path)) {
                            std::ofstream dummy(dummy_path);
                            dummy << "// macman: dummy for macOS BSD ar compatibility\n";
                            dummy << "namespace { int macman_dummy_symbol = 0; }\n";
                            dummy.close();
                        }

                        std::string target = "add_library(" + lib_name;
                        size_t pos = content.find(target);
                        if (pos != std::string::npos) {
                            int depth = 0;
                            size_t paren_start = content.find('(', pos);
                            for (size_t i = paren_start; i < content.size(); i++) {
                                if (content[i] == '(') depth++;
                                if (content[i] == ')') {
                                    depth--;
                                    if (depth == 0) {
                                        content.insert(i, " ${CMAKE_CURRENT_SOURCE_DIR}/macman_dummy.cpp");
                                        break;
                                    }
                                }
                            }
                            std::ofstream f(entry.path());
                            f << content;
                            any_fix_applied = true;
                        }
                        break;
                    }
                }
            }
        }
    }

    if (any_fix_applied) {
        std::ofstream compat_out(compat_header);
        compat_out << compat_content;
        compat_out.close();

        if (env_setup.find(compat_dir) == std::string::npos) {
            std::string inject = " -I'" + compat_dir + "' -include '" + compat_header + "'";

            auto inject_into_var = [&](const std::string& var) {
                size_t pos = env_setup.find(var + "='");
                if (pos != std::string::npos) {
                    size_t quote_end = env_setup.find("'", pos + var.length() + 2);
                    if (quote_end != std::string::npos) {
                        env_setup.insert(quote_end, inject);
                    }
                }
            };

            inject_into_var("CFLAGS");
            inject_into_var("CXXFLAGS");
        }
    }

    return any_fix_applied;
}

} // namespace macman
