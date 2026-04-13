// aur_backend.cpp — Arch User Repository Source Builder Implementation
// Fallback backend: queries AUR RPC API, downloads PKGBUILDs, and
// compiles packages from source using system clang for native Mach-O.
// v2 improvements:
// - In-memory TTL cache for search/info (5 min) — instant repeated lookups
// - Self-healing build engine: captures build output, matches ~20 known
// Linux→macOS error patterns, auto-patches source, retries up to 3x
// - 3-level macOS compatibility system with red Linux-only warnings
// - Extended macOS wrapper script (sed, readlink, nproc, sha256sum, etc.)


#include "backend/aur_backend.hpp"
#include "macman.hpp"
#include "core/config.hpp"
#include "cli/colors.hpp"
#include "ui/progress_bar.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>
#include <cstdlib>
#include <array>
#include <iostream>

namespace fs = std::filesystem;

namespace macman {

// --- Constructor ---

AURBackend::AURBackend()
    : build_dir_(get_cache_dir() + "/builds") {
    try {
        if (!fs::exists(build_dir_)) {
            fs::create_directories(build_dir_);
        }
    } catch (...) {}
}

// --- Cache Validity Check ---

bool AURBackend::is_cache_valid(time_t timestamp) const {
    return (std::time(nullptr) - timestamp) < CACHE_TTL_SECONDS;
}

// --- Search AUR (Cached) ---

std::vector<Package> AURBackend::search(const std::string& query) {
    // Check cache first
    auto cache_it = search_cache_.find(query);
    if (cache_it != search_cache_.end() && is_cache_valid(cache_it->second.timestamp)) {
        return cache_it->second.results;
    }

    std::vector<Package> results;

    // Query AUR RPC API
    std::string url = std::string(AUR_RPC_BASE) + "?v=5&type=search&arg=" + query;
    
    auto response = http_.get_json(url);
    if (!response.success) {
        return results;
    }

    try {
        auto json = nlohmann::json::parse(response.body);
        
        if (json.contains("results") && json["results"].is_array()) {
            for (const auto& result : json["results"]) {
                results.push_back(aur_json_to_package(result));
                if (results.size() >= 25) break; // Limit results
            }
        }
    } catch (...) {}

    // Cache the results
    search_cache_[query] = {results, std::time(nullptr)};

    return results;
}

// --- Get Package Info (Cached) ---

std::optional<Package> AURBackend::get_info(const std::string& name) {
    // Check cache first
    auto cache_it = info_cache_.find(name);
    if (cache_it != info_cache_.end() && is_cache_valid(cache_it->second.timestamp)) {
        return cache_it->second.pkg;
    }

    std::string url = std::string(AUR_RPC_BASE) + "?v=5&type=info&arg[]=" + name;
    
    auto response = http_.get_json(url);
    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = nlohmann::json::parse(response.body);
        if (json.contains("results") && json["results"].is_array() && 
            !json["results"].empty()) {
            Package pkg = aur_json_to_package(json["results"][0]);
            // Cache the result
            info_cache_[name] = {pkg, std::time(nullptr)};
            return pkg;
        }
    } catch (...) {}

    return std::nullopt;
}

// --- Convert AUR JSON to Package ---

Package AURBackend::aur_json_to_package(const nlohmann::json& result) const {
    Package pkg;
    
    pkg.name        = result.value("Name", "");
    pkg.version     = result.value("Version", "");
    pkg.description = result.value("Description", "");
    pkg.homepage    = result.value("URL", "");
    pkg.source      = PackageSource::AUR;

    auto add_deps = [&](const nlohmann::json& source_array, std::vector<std::string>& target_list) {
        std::vector<std::string> ignore_deps = {
            "glibc", "linux-headers", "linux-api-headers", "bash", "gcc"
        };
        
        if (source_array.is_array()) {
            for (const auto& dep : source_array) {
                if (dep.is_string()) {
                    std::string dep_str = dep.get<std::string>();
                    auto pos = dep_str.find_first_of(">=<");
                    if (pos != std::string::npos) {
                        dep_str = dep_str.substr(0, pos);
                    }
                    
                    if (std::find(ignore_deps.begin(), ignore_deps.end(), dep_str) == ignore_deps.end()) {
                        target_list.push_back(dep_str);
                    }
                }
            }
        }
    };

    if (result.contains("Depends")) add_deps(result["Depends"], pkg.dependencies);
    if (result.contains("MakeDepends")) add_deps(result["MakeDepends"], pkg.build_deps);

    return pkg;
}

// --- Download PKGBUILD ---

std::optional<PKGBUILDInfo> AURBackend::download_pkgbuild(const std::string& name) {
    // Download AUR snapshot
    std::string snapshot_url = std::string(AUR_PACKAGE_BASE) + name + ".tar.gz";
    std::string snapshot_path = build_dir_ + "/" + name + ".tar.gz";
    std::string extract_dir = build_dir_ + "/" + name;

    colors::print_substatus("Downloading PKGBUILD for " + name + "...");

    if (!http_.download_file(snapshot_url, snapshot_path)) {
        colors::print_error("Failed to download PKGBUILD for " + name);
        return std::nullopt;
    }

    // Extract
    if (fs::exists(extract_dir)) {
        fs::remove_all(extract_dir);
    }
    fs::create_directories(extract_dir);

    std::string cmd = "tar xzf '" + snapshot_path + "' -C '" + extract_dir + "' --strip-components=1";
    if (system(cmd.c_str()) != 0) {
        colors::print_error("Failed to extract PKGBUILD archive (or unsupported tar format)");
        return std::nullopt;
    }

    // Read PKGBUILD
    std::string pkgbuild_path = extract_dir + "/PKGBUILD";
    if (!fs::exists(pkgbuild_path)) {
        colors::print_error("PKGBUILD not found in " + name + " snapshot");
        return std::nullopt;
    }

    return parse_pkgbuild(pkgbuild_path);
}

// --- Parse PKGBUILD File (Native Bash Dumper) ---

PKGBUILDInfo AURBackend::parse_pkgbuild(const std::string& pkgbuild_path) const {
    PKGBUILDInfo info;

    // We write a temporary bash script that correctly sources the PKGBUILD 
    // and outputs variables line-by-line to avoid regex guessing errors like ${url}.
    std::string dumper_script = R"BASH(
#!/bin/bash
source "$1" 2>/dev/null
echo "PKGNAME"
echo "${pkgname}"
echo "PKGVER"
echo "${pkgver}"
echo "PKGREL"
echo "${pkgrel}"
echo "PKGDESC"
echo "${pkgdesc}"
echo "URL"
echo "${url}"

echo "SOURCE"
for s in "${source[@]}"; do echo "$s"; done
echo "DEPENDS"
for d in "${depends[@]}"; do echo "$d"; done
echo "MAKEDEPENDS"
for m in "${makedepends[@]}"; do echo "$m"; done
echo "END"
)BASH";

    std::string script_path = build_dir_ + "/macman_dumper.sh";
    std::ofstream out(script_path);
    out << dumper_script;
    out.close();
    system(("chmod +x '" + script_path + "'").c_str());

    std::string cmd = "bash " + script_path + " '" + pkgbuild_path + "'";
    FILE* pipe = popen(cmd.c_str(), "r");
    std::vector<std::string> lines;
    if (pipe) {
        char buffer[1024];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            std::string line = buffer;
            if (!line.empty() && line.back() == '\n') line.pop_back();
            lines.push_back(line);
        }
        pclose(pipe);
    }
    fs::remove(script_path);

    // Parse the output lines
    std::string current_state = "";
    for (const auto& line : lines) {
        if (line == "PKGNAME" || line == "PKGVER" || line == "PKGREL" || 
            line == "PKGDESC" || line == "URL" || line == "SOURCE" || 
            line == "DEPENDS" || line == "MAKEDEPENDS" || line == "END") {
            current_state = line;
            continue;
        }

        if (current_state == "PKGNAME") info.pkgname = line;
        else if (current_state == "PKGVER") info.pkgver = line;
        else if (current_state == "PKGREL") info.pkgrel = line;
        else if (current_state == "PKGDESC") info.pkgdesc = line;
        else if (current_state == "URL") info.url = line;
        else if (current_state == "SOURCE") info.source.push_back(line);
        else if (current_state == "DEPENDS") info.depends.push_back(line);
        else if (current_state == "MAKEDEPENDS") info.makedepends.push_back(line);
    }

    // Now extract the raw build() and package() functions from the file text.
    std::ifstream file(pkgbuild_path);
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    auto extract_function = [&](const std::string& fname) -> std::string {
        std::string marker = fname + "()";
        auto pos = content.find(marker);
        if (pos == std::string::npos) {
            marker = fname + " ()";
            pos = content.find(marker);
        }
        if (pos == std::string::npos) return "";

        auto brace_start = content.find('{', pos);
        if (brace_start == std::string::npos) return "";

        int depth = 0;
        size_t end = brace_start;
        for (size_t i = brace_start; i < content.size(); i++) {
            if (content[i] == '{') depth++;
            if (content[i] == '}') depth--;
            if (depth == 0) { end = i; break; }
        }

        return content.substr(brace_start + 1, end - brace_start - 1);
    };

    info.build_commands   = extract_function("build");
    info.package_commands = extract_function("package");

    return info;
}

// --- macOS Compatibility Check (3-Level) ---

CompatLevel AURBackend::check_macos_compatibility(const PKGBUILDInfo& info) const {
    // Hard Linux-only dependencies → LINUX_ONLY
    std::vector<std::string> linux_only_deps = {
        "glibc", "linux-headers", "linux-api-headers", "linux-firmware",
        "systemd", "libsystemd", "eudev", "udev",
        "alsa-lib", "alsa-utils", "pulseaudio", "pipewire",
        "wayland", "wayland-protocols", "wlroots",
        "libdrm", "mesa", "vulkan-icd-loader",
        "kernel", "kmod", "module-init-tools"
    };

    // Partially compatible — macOS has alternatives
    std::vector<std::string> partial_deps = {
        "dbus", "polkit", "pam", "libcap", "inotify-tools",
        "libseccomp", "apparmor", "selinux"
    };

    bool has_linux_only = false;
    bool has_partial = false;

    auto check_dep_list = [&](const std::vector<std::string>& deps) {
        for (const auto& dep : deps) {
            for (const auto& bad : linux_only_deps) {
                if (dep.find(bad) != std::string::npos) {
                    has_linux_only = true;
                }
            }
            for (const auto& warn : partial_deps) {
                if (dep.find(warn) != std::string::npos) {
                    has_partial = true;
                }
            }
        }
    };

    check_dep_list(info.depends);
    check_dep_list(info.makedepends);

    // Also check build/package commands for Linux-specific patterns
    std::string all_commands = info.build_commands + " " + info.package_commands;
    std::vector<std::string> linux_only_patterns = {
        "/proc/", "/sys/class/", "systemctl", "journalctl",
        "CONFIG_LINUX", "modprobe", "insmod", "depmod",
        "dkms", "/etc/systemd/", "udevadm"
    };

    for (const auto& pattern : linux_only_patterns) {
        if (all_commands.find(pattern) != std::string::npos) {
            has_linux_only = true;
        }
    }

    if (has_linux_only) return CompatLevel::LINUX_ONLY;
    if (has_partial)    return CompatLevel::PARTIAL;
    return CompatLevel::COMPATIBLE;
}

// --- Get Incompatibility Reason (Human-Readable) ---

std::string AURBackend::get_incompatibility_reason(const PKGBUILDInfo& info) const {
    std::vector<std::string> reasons;

    std::map<std::string, std::string> dep_reasons = {
        {"glibc",           "requires GNU C Library (macOS uses libSystem)"},
        {"linux-headers",   "requires Linux kernel headers"},
        {"linux-api-headers","requires Linux kernel API headers"},
        {"linux-firmware",  "requires Linux firmware blobs"},
        {"systemd",         "requires systemd init system"},
        {"libsystemd",      "links against libsystemd"},
        {"eudev",           "requires eudev device manager"},
        {"udev",            "requires udev device manager"},
        {"alsa-lib",        "requires ALSA (Linux-only audio API)"},
        {"pulseaudio",      "requires PulseAudio (use CoreAudio on macOS)"},
        {"wayland",         "requires Wayland display server (Linux-only)"},
        {"libdrm",          "requires Linux Direct Rendering Manager"},
        {"kmod",            "requires Linux kernel module tools"}
    };

    auto check = [&](const std::vector<std::string>& deps) {
        for (const auto& dep : deps) {
            for (const auto& [key, reason] : dep_reasons) {
                if (dep.find(key) != std::string::npos) {
                    reasons.push_back(reason);
                }
            }
        }
    };

    check(info.depends);
    check(info.makedepends);

    // Check command patterns
    std::string all_commands = info.build_commands + " " + info.package_commands;
    if (all_commands.find("systemctl") != std::string::npos)
        reasons.push_back("uses systemctl service manager");
    if (all_commands.find("/proc/") != std::string::npos)
        reasons.push_back("reads from /proc filesystem (Linux-only)");
    if (all_commands.find("modprobe") != std::string::npos)
        reasons.push_back("loads Linux kernel modules");

    // Deduplicate
    std::sort(reasons.begin(), reasons.end());
    reasons.erase(std::unique(reasons.begin(), reasons.end()), reasons.end());

    if (reasons.empty()) return "Unknown Linux-specific dependency";

    std::string result;
    for (size_t i = 0; i < reasons.size() && i < 3; i++) {
        if (i > 0) result += ", ";
        result += reasons[i];
    }
    if (reasons.size() > 3) {
        result += " (+" + std::to_string(reasons.size() - 3) + " more)";
    }
    return result;
}

// --- Download Source Files ---

bool AURBackend::download_sources(const PKGBUILDInfo& info, const std::string& work_dir) {
    for (const auto& src : info.source) {
        // Variables like $url and ${pkgname} are already naturally evaluated by our Bash Dumper!
        std::string url = src;
        url.erase(std::remove(url.begin(), url.end(), '"'), url.end());
        url.erase(std::remove(url.begin(), url.end(), '\''), url.end());

        // Extract custom directory name from "name::url" syntax (makepkg convention)
        std::string custom_name;
        std::string raw_url = url;
        if (url.find("::") != std::string::npos) {
            custom_name = url.substr(0, url.find("::"));
            raw_url = url.substr(url.find("::") + 2);
        }

        std::string cmd;
        if (raw_url.find("git+") == 0 || raw_url.find("git://") == 0) {
            std::string git_url = raw_url;
            if (git_url.find("git+") == 0) git_url = git_url.substr(4);
            
            // Clean branch fragments
            if (git_url.find("#") != std::string::npos) {
                git_url = git_url.substr(0, git_url.find("#"));
            }
            
            // Use custom_name as clone target dir (makepkg does the same)
            std::string clone_target = custom_name.empty() ? "" : " '" + custom_name + "'";
            
            colors::print_substatus("Cloning git repository: " + git_url);
            cmd = "cd '" + work_dir + "' && git clone --depth 1 '" + git_url + "'" + clone_target + " 2>/dev/null";
        } else {
            colors::print_substatus("Downloading source: " + raw_url);
            cmd = "cd '" + work_dir + "' && curl -L -O -s '" + raw_url + "'";
        }

        int ret = system(cmd.c_str());
        if (ret != 0) {
            colors::print_error("Failed to download or clone: " + raw_url);
            return false;
        }
    }

    return true;
}

// --- Known Build Error Fixes ---

std::vector<BuildError> AURBackend::get_known_fixes() const {
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
        {"malloc_usable_size", "malloc_usable_size() not on macOS (use malloc_size)",
         "define", "#ifdef __APPLE__\n#include <malloc/malloc.h>\n#define malloc_usable_size(p) malloc_size(p)\n#endif"},

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
    };
}

// --- Run Build Command and Capture Output ---

int AURBackend::run_build_capturing_output(const std::string& cmd, std::string& output) {
    output.clear();
    // Redirect both stdout and stderr to a temp file so we can parse errors
    std::string log_file = build_dir_ + "/build_output.log";
    std::string full_cmd = cmd + " > '" + log_file + "' 2>&1";
    
    int ret = system(full_cmd.c_str());
    
    // Read the log file
    try {
        std::ifstream file(log_file);
        if (file.is_open()) {
            output = std::string((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
        }
    } catch (...) {}

    return ret;
}

// --- Analyze Build Log and Apply Fixes ---

bool AURBackend::analyze_and_fix_build(const std::string& build_log,
                                        const std::string& work_dir,
                                        const std::string& src_dir,
                                        std::string& env_setup) {
    auto known_fixes = get_known_fixes();
    bool any_fix_applied = false;

    // Create a compat header directory for stub headers
    std::string compat_dir = work_dir + "/macman_compat";
    fs::create_directories(compat_dir);

    // Path for our macman compat header that gets force-included
    std::string compat_header = compat_dir + "/macman_compat.h";
    
    // Read existing compat header if it exists
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
            // Create empty stub header
            if (!fix.fix_value.empty()) {
                fs::path header_path = fs::path(compat_dir) / fix.fix_value;
                fs::create_directories(header_path.parent_path());
                if (!fs::exists(header_path)) {
                    std::ofstream stub(header_path.string());
                    stub << "/* macman auto-stub: " << fix.fix_value << " */\n";
                    stub << "/* This header is not available on macOS. */\n";
                    stub.close();
                    any_fix_applied = true;
                }
            } else {
                // Generic linux/ header — extract the exact header name from build log
                std::regex header_re("fatal error: '(linux/[^']+)' file not found");
                std::smatch match;
                if (std::regex_search(build_log, match, header_re)) {
                    std::string header_name = match[1].str();
                    fs::path header_path = fs::path(compat_dir) / header_name;
                    fs::create_directories(header_path.parent_path());
                    if (!fs::exists(header_path)) {
                        std::ofstream stub(header_path.string());
                        stub << "/* macman auto-stub: " << header_name << " */\n";
                        stub << "/* This Linux kernel header is not available on macOS. */\n";
                        stub.close();
                        any_fix_applied = true;
                    }
                }
            }
        }
        else if (fix.fix_type == "define" || fix.fix_type == "typedef") {
            // Add to compat header if not already present
            if (compat_content.find(fix.pattern) == std::string::npos) {
                compat_content += "\n" + fix.fix_value + "\n";
                any_fix_applied = true;
            }
        }
        else if (fix.fix_type == "ldflag_remove") {
            // Remove the problematic flag from LDFLAGS/CFLAGS in env_setup
            std::string flag = fix.fix_value;
            size_t pos;
            while ((pos = env_setup.find(flag)) != std::string::npos) {
                env_setup.erase(pos, flag.length());
                any_fix_applied = true;
            }

            // Also try to patch Makefile/CMakeLists.txt in the source
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
    }

    // Write the compat header
    if (any_fix_applied) {
        std::ofstream compat_out(compat_header);
        compat_out << compat_content;
        compat_out.close();

        // Add compat directory to include path and force-include the compat header
        if (env_setup.find(compat_dir) == std::string::npos) {
            // Inject -I and -include into CFLAGS/CXXFLAGS
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

// --- Compile Source (Self-Healing with Retry) ---

bool AURBackend::compile_source(const PKGBUILDInfo& info, const std::string& work_dir,
                                const std::string& install_prefix,
                                std::vector<std::string>& installed_files) {
    colors::print_substatus("Configuring build environment for " + info.pkgname + " " + info.pkgver + "...");
    
    // Find the source directory (usually the extracted archive creates a dir)
    std::string src_dir = work_dir;
    for (const auto& entry : fs::directory_iterator(work_dir)) {
        if (entry.is_directory()) {
            std::string dir_name = entry.path().filename().string();
            if (dir_name == "macman_compat") continue; // Skip our compat dir
            // If the directory name is a substring of pkgname (or vice versa), or if it contains build files, it's our source!
            if (info.pkgname.find(dir_name) != std::string::npos || 
                dir_name.find(info.pkgname) != std::string::npos ||
                fs::exists(entry.path().string() + "/CMakeLists.txt") ||
                fs::exists(entry.path().string() + "/configure") ||
                fs::exists(entry.path().string() + "/Makefile") ||
                fs::exists(entry.path().string() + "/meson.build") ||
                fs::exists(entry.path().string() + "/setup.py")) {
                src_dir = entry.path().string();
                break;
            }
        }
    }

    // Set up environment for macOS compilation
    std::string env_setup = "export CC=clang CXX=clang++ "
                           "CFLAGS='-O2 -I/opt/homebrew/include -I/usr/local/include' "
                           "CXXFLAGS='-O2 -I/opt/homebrew/include -I/usr/local/include' "
                           "LDFLAGS='' "
                           "PKG_CONFIG_PATH='/opt/homebrew/lib/pkgconfig:/usr/local/lib/pkgconfig' "
                           "EXPAT_LIBPATH='/usr/lib' "
                           "PREFIX='" + install_prefix + "' && ";

    // Containerization Concept: DESTDIR Staging Directory
    std::string destdir = build_dir_ + "/destdir-" + info.pkgname;
    if (fs::exists(destdir)) {
        fs::remove_all(destdir);
    }
    fs::create_directories(destdir);
    // Pre-create standard Unix directories to prevent 'cp' errors in badly written Makefiles
    fs::create_directories(destdir + "/usr/local/bin");
    fs::create_directories(destdir + "/usr/local/lib");
    fs::create_directories(destdir + "/usr/local/include");
    fs::create_directories(destdir + "/usr/local/share/man/man1");
    fs::create_directories(destdir + "/opt");

    // Extended macOS compatibility wrapper script
    std::string wrapper_script = build_dir_ + "/macman_makepkg_wrapper.sh";
    std::string script_content = R"BASH(#!/bin/bash
set -e

# ── macOS Compatibility Wrappers ─────────────────────────────────────────────

# GNU install -D → macOS install with mkdir -p
install() {
    local args=()
    local create_dirs=false
    for arg in "$@"; do
        if [[ "$arg" == "-D" ]]; then
            create_dirs=true
        elif [[ "$arg" == "--mode" ]]; then
            args+=("-m")
        elif [[ "$arg" == "-t" ]]; then
            args+=("-d")
        else
            args+=("$arg")
        fi
    done
    if $create_dirs; then
        local target="${args[-1]}"
        command mkdir -p "$(dirname "$target")"
    fi
    /usr/bin/install "${args[@]}"
}

# BSD cp (handle GNU long options)
cp() {
    local args=()
    for arg in "$@"; do
        if [[ "$arg" == "--reflink=auto" || "$arg" == "--reflink" ]]; then continue
        elif [[ "$arg" == "--no-preserve=ownership" ]]; then continue
        else args+=("$arg")
        fi
    done
    command cp "${args[@]}"
}

# mkdir --parents → -p
mkdir() {
    local args=()
    for arg in "$@"; do
        if [[ "$arg" == "--parents" ]]; then args+=("-p")
        else args+=("$arg")
        fi
    done
    command mkdir "${args[@]}"
}

# ln --symbolic → -s, --force → -f, --relative → manual
ln() {
    local args=()
    for arg in "$@"; do
        if [[ "$arg" == "--symbolic" ]]; then args+=("-s")
        elif [[ "$arg" == "--force" ]]; then args+=("-f")
        elif [[ "$arg" == "--relative" ]]; then continue  # Skip, not supported on macOS
        else args+=("$arg")
        fi
    done
    command ln "${args[@]}"
}

# sed -i '' (BSD) vs sed -i (GNU) — handle GNU-style in-place edit
sed() {
    local args=()
    local i=0
    while [[ $# -gt 0 ]]; do
        if [[ "$1" == "-i" ]]; then
            args+=("-i" "")
            # If next arg looks like a sed expression (starts with s/ or /), don't consume it
            if [[ $# -gt 1 && ! "$2" =~ ^$ && ! "$2" =~ ^[/s] ]]; then
                shift  # Skip the GNU backup suffix
            fi
        else
            args+=("$1")
        fi
        shift
    done
    command sed "${args[@]}"
}

# readlink -f → custom implementation (macOS readlink doesn't support -f)
readlink() {
    local args=()
    local do_canonical=false
    for arg in "$@"; do
        if [[ "$arg" == "-f" || "$arg" == "--canonicalize" ]]; then
            do_canonical=true
        else
            args+=("$arg")
        fi
    done
    if $do_canonical; then
        python3 -c "import os,sys; print(os.path.realpath(sys.argv[1]))" "${args[-1]}" 2>/dev/null || command readlink "${args[@]}"
    else
        command readlink "${args[@]}"
    fi
}

# nproc → sysctl
nproc() {
    sysctl -n hw.ncpu
}

# sha256sum → shasum -a 256
sha256sum() {
    shasum -a 256 "$@"
}

# md5sum → md5
md5sum() {
    md5 "$@"
}

# Export macOS-safe environment
# srcdir = work directory (where sources are extracted/cloned)
# This matches makepkg behavior — PKGBUILDs navigate into subdirs themselves
export srcdir=")BASH" + work_dir + R"BASH("
export pkgdir=")BASH" + destdir + R"BASH("
export pkgname=")BASH" + info.pkgname + R"BASH("
export pkgver=")BASH" + info.pkgver + R"BASH("

cd "$srcdir"

build() {
    :
)BASH" + info.build_commands + R"BASH(
}

package() {
    :
)BASH" + info.package_commands + R"BASH(
}

if [ -n ")BASH" + info.build_commands + R"BASH(" ]; then
    build
fi

if [ -n ")BASH" + info.package_commands + R"BASH(" ]; then
    package
fi
)BASH";

    // --- Self-Healing Build Loop ---

    int ret = -1;
    std::string build_output;

    for (int attempt = 0; attempt < MAX_BUILD_RETRIES; attempt++) {
        // Write the wrapper script
        {
            std::ofstream out(wrapper_script);
            out << script_content;
            out.close();
            // Use chmod instead of fs::permissions to avoid macOS permission exceptions
            system(("chmod +x '" + wrapper_script + "'").c_str());
        }

        std::string build_cmd = env_setup + wrapper_script;

        if (attempt == 0) {
            colors::print_substatus("Compiling with macOS native toolchain...");
        } else {
            colors::print_substatus("Retry " + std::to_string(attempt) + "/" + 
                                   std::to_string(MAX_BUILD_RETRIES - 1) + 
                                   ": Recompiling with applied fixes...");
        }

        ret = run_build_capturing_output(build_cmd, build_output);
        
        if (ret == 0) {
            // Build succeeded!
            if (attempt > 0) {
                colors::print_success("Build succeeded after self-healing (" + 
                                     std::to_string(attempt) + " fix round" + 
                                     (attempt > 1 ? "s" : "") + " applied)");
            }
            break;
        }

        // Build failed — try to fix
        if (attempt < MAX_BUILD_RETRIES - 1) {
            colors::print_warning("Build failed. Analyzing errors...");
            
            bool fixed = analyze_and_fix_build(build_output, work_dir, src_dir, env_setup);
            
            if (!fixed) {
                colors::print_error("Build failed and no automatic fix could be applied.");
                // Print last few lines of build output for debugging
                std::istringstream stream(build_output);
                std::vector<std::string> log_lines;
                std::string line;
                while (std::getline(stream, line)) {
                    log_lines.push_back(line);
                }
                size_t start = log_lines.size() > 15 ? log_lines.size() - 15 : 0;
                colors::print_substatus("Last lines of build output:");
                for (size_t i = start; i < log_lines.size(); i++) {
                    std::cerr << "  " << log_lines[i] << std::endl;
                }
                return false;
            }
        }
    }

    if (ret != 0) {
        colors::print_error("Build failed after " + std::to_string(MAX_BUILD_RETRIES) + " attempts.");
        // Show last lines of build output
        std::istringstream stream(build_output);
        std::vector<std::string> log_lines;
        std::string line;
        while (std::getline(stream, line)) {
            log_lines.push_back(line);
        }
        size_t start = log_lines.size() > 20 ? log_lines.size() - 20 : 0;
        colors::print_substatus("Build output (last 20 lines):");
        for (size_t i = start; i < log_lines.size(); i++) {
            std::cerr << "  " << log_lines[i] << std::endl;
        }
        return false;
    }

    // Process DESTDIR: Track files and deploy to install_prefix
    // Remap system paths: /usr/bin/X → prefix/bin/X, /usr/local/bin/X → prefix/bin/X
    // macOS SIP protects /usr/bin, so we redirect everything to user-controlled prefix
    colors::print_substatus("Containerizing package and tracking files...");
    try {
        if (!fs::exists(destdir) || fs::is_empty(destdir)) {
            colors::print_error("No files were installed to DESTDIR during build.");
            return false;
        }

        // Collect all files from DESTDIR first (avoid iterator invalidation)
        std::vector<std::string> staged_files;
        for (const auto& entry : fs::recursive_directory_iterator(destdir)) {
            if (entry.is_regular_file() || entry.is_symlink()) {
                staged_files.push_back(entry.path().string());
            }
        }

        for (const auto& staged_path : staged_files) {
            // Calculate relative path from DESTDIR
            std::string relative_path = staged_path.substr(destdir.length()); 
            
            // --- Path Remapping Logic ---
            auto remap_sys_path = [&](const std::string& path) {
                if (path.find("/usr/local/") == 0) return install_prefix + path.substr(10);
                if (path.find("/usr/") == 0)       return install_prefix + path.substr(4);
                if (path.find("/opt/") == 0)       return install_prefix + path;
                if (path.find("/") == 0)           return install_prefix + path; // Fallback for absolute paths
                return path; // Keep relative paths as is
            };
            
            std::string remapped_path = remap_sys_path(relative_path);
            
            // Use system commands — far more reliable on macOS than C++ fs::copy
            fs::path target_dir = fs::path(remapped_path).parent_path();
            system(("mkdir -p '" + target_dir.string() + "'").c_str());

            if (fs::is_symlink(staged_path)) {
                // Read the symlink target
                std::string target = fs::read_symlink(staged_path).string();
                
                // If it's an absolute symlink pointing to /usr/..., remap it too!
                if (!target.empty() && target[0] == '/') {
                    target = remap_sys_path(target);
                }
                
                // Recreate symlink using ln -sf
                system(("ln -sf '" + target + "' '" + remapped_path + "'").c_str());
            } else {
                // Copy normal file
                int cp_ret = system(("cp -f '" + staged_path + "' '" + remapped_path + "' 2>/dev/null || "
                                     "cp -Rf '" + staged_path + "' '" + remapped_path + "' 2>/dev/null").c_str());
                
                if (cp_ret != 0) {
                    colors::print_warning("Could not install: " + remapped_path + " (skipping)");
                    continue;
                }

                // Make binaries executable
                if (remapped_path.find("/bin/") != std::string::npos) {
                    system(("chmod +x '" + remapped_path + "' 2>/dev/null").c_str());
                }
            }

            installed_files.push_back(remapped_path);
            colors::print_substatus("Installed: " + remapped_path);
        }
        
        // Cleanup staging directory
        fs::remove_all(destdir);
        
    } catch (const std::exception& e) {
        colors::print_error("Failed to deploy package files: " + std::string(e.what()));
        return false;
    }

    return true;
}

// --- Collect Installed Files ---

std::vector<std::string> AURBackend::collect_installed_files(const std::string& prefix) const {
    std::vector<std::string> files;
    try {
        for (const auto& entry : fs::recursive_directory_iterator(prefix)) {
            if (entry.is_regular_file() || entry.is_symlink()) {
                files.push_back(entry.path().string());
            }
        }
    } catch (...) {}
    return files;
}

// --- Build and Install (with Linux-Only Warning) ---

bool AURBackend::build_and_install(const std::string& name, const std::string& install_prefix,
                                   std::vector<std::string>& installed_files) {
    // Step 1: Download PKGBUILD
    auto pkgbuild = download_pkgbuild(name);
    if (!pkgbuild) {
        return false;
    }

    // Step 2: Check macOS compatibility (3-level system)
    CompatLevel compat = check_macos_compatibility(*pkgbuild);
    
    if (compat == CompatLevel::LINUX_ONLY) {
        std::string reason = get_incompatibility_reason(*pkgbuild);
        
        std::cout << std::endl;
        std::cerr << colors::BOLD_RED 
                  << "  ⚠  WARNING: This package is designed for the Linux kernel only."
                  << colors::RESET << std::endl;
        std::cerr << colors::BOLD_RED 
                  << "     It uses Linux-specific APIs that cannot run on macOS."
                  << colors::RESET << std::endl;
        std::cerr << colors::RED 
                  << "     Reason: " << reason
                  << colors::RESET << std::endl;
        std::cout << std::endl;
        std::cout << colors::BOLD_YELLOW 
                  << "  Do you want to attempt the build anyway? [y/N] " 
                  << colors::RESET;

        std::string input;
        std::getline(std::cin, input);
        
        if (input != "y" && input != "Y" && input != "yes") {
            colors::print_substatus("Build cancelled by user.");
            return false;
        }
        std::cout << std::endl;
    } 
    else if (compat == CompatLevel::PARTIAL) {
        colors::print_warning(name + " has partial macOS compatibility. Some features may not work.");
        colors::print_warning("Attempting build with macOS native toolchain...");
    }

    // Step 3: Create work directory
    std::string work_dir = build_dir_ + "/" + name + "-build";
    if (fs::exists(work_dir)) {
        fs::remove_all(work_dir);
    }
    fs::create_directories(work_dir);

    // Step 4: Download source files
    if (!download_sources(*pkgbuild, work_dir)) {
        return false;
    }

    // Step 5: Compile (with self-healing retry loop)
    if (!compile_source(*pkgbuild, work_dir, install_prefix, installed_files)) {
        return false;
    }

    colors::print_success("Package built and deployed successfully (" + 
                          std::to_string(installed_files.size()) + " files tracked).");

    // Clean up build directory
    try {
        fs::remove_all(work_dir);
    } catch (...) {}

    return true;
}

// --- Check Package Availability ---

bool AURBackend::has_package(const std::string& name) {
    auto info = get_info(name);
    return info.has_value();
}

} // namespace macman
