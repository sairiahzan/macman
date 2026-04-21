// aur_backend.cpp — Arch User Repository Source Builder Implementation [V1.1.0 Patch]
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
#include "core/process.hpp"
#include "ui/progress_bar.hpp"
#include <filesystem>
#include <sys/stat.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>
#include <set>
#include <cstdlib>
#include <iostream>

namespace fs = std::filesystem;

namespace macman {

// --- Constructor ---

AURBackend::AURBackend()
    : build_dir_(get_cache_dir() + "/builds"),
      healing_engine_(build_dir_) {
    try {
        // Destroy entire build directory to clear compiler caches (e.g. config.cache)
        // Autotools will aggressively reload dead CFLAGS if this persists between attempts.
        if (fs::exists(build_dir_)) {
            fs::remove_all(build_dir_);
        }
        fs::create_directories(build_dir_);
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

    auto response = http_.download_file(snapshot_url, snapshot_path);
    if (!response.success) {
        colors::print_error("Failed to download PKGBUILD for " + name + " (" + response.error + ")");
        return std::nullopt;
    }

    // Extract
    if (fs::exists(extract_dir)) {
        fs::remove_all(extract_dir);
    }
    fs::create_directories(extract_dir);

    if (run_exec("/usr/bin/tar", {"xzf", snapshot_path, "-C", extract_dir, "--strip-components=1"}, false) != 0) {
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
    ::chmod(script_path.c_str(), 0755);

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

        // Replace $url with the package's URL if present
        if (raw_url.find("$url") != std::string::npos) {
            size_t p = raw_url.find("$url");
            raw_url.replace(p, 4, info.url);
        }

        // Ensure work_dir exists and is writable
        if (!fs::exists(work_dir)) {
            fs::create_directories(work_dir);
        }

        int ret;
        if (raw_url.find("git+") == 0 || raw_url.find("git://") == 0) {
            std::string git_url = raw_url;
            if (git_url.find("git+") == 0) git_url = git_url.substr(4);
            
            // Clean branch fragments
            if (git_url.find("#") != std::string::npos) {
                git_url = git_url.substr(0, git_url.find("#"));
            }
            
            colors::print_substatus("Cloning git repository: " + git_url);
            std::vector<std::string> git_args = {"clone", "--depth", "1", git_url};
            if (!custom_name.empty()) git_args.push_back(custom_name);
            ret = run_exec("/usr/bin/git", git_args, true, work_dir);
        } else {
            colors::print_substatus("Downloading source: " + (custom_name.empty() ? raw_url : custom_name));
            std::vector<std::string> curl_args = {"-L", "-s"};
            if (custom_name.empty()) {
                curl_args.push_back("-O");
            } else {
                curl_args.push_back("-o");
                curl_args.push_back(custom_name);
            }
            curl_args.push_back(raw_url);
            ret = run_exec("/usr/bin/curl", curl_args, false, work_dir);
        }

        if (ret != 0) {
            colors::print_error("Failed to download or clone: " + raw_url);
            return false;
        }
    }

    return true;
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

    // Detect Homebrew prefix
    std::string brew_prefix = fs::exists("/opt/homebrew/bin") ? "/opt/homebrew" : "/usr/local";

    // Pre-build: Install essential build tools if missing
    // Homebrew refuses to run as root — detect SUDO_USER and run brew as them
    const char* sudo_user = std::getenv("SUDO_USER");
    
    std::vector<std::pair<std::string, std::string>> essential_tools = {
        {"pkg-config", "pkgconf"},  // modern Homebrew installs pkgconf which provides pkg-config
        {"cmake", "cmake"}
    };
    for (const auto& [tool_bin, brew_name] : essential_tools) {
        std::string tool_path = brew_prefix + "/bin/" + tool_bin;
        if (!fs::exists(tool_path)) {
            std::string brew_bin = brew_prefix + "/bin/brew";
            if (sudo_user) {
                run_exec("/usr/bin/sudo", {"-u", sudo_user, brew_bin, "install", brew_name});
            } else {
                run_exec(brew_bin, {"install", brew_name});
            }
        }
    }

    // Set up environment for macOS compilation
    std::string extra_cflags, extra_ldflags, extra_pkgconfig, extra_libs;
    std::vector<std::string> potential_kegs = {"e2fsprogs", "openssl", "icu4c", "libxml2", "libedit", "ncurses"};
    for (const auto& keg : potential_kegs) {
        // Check both Homebrew cellar and macman's own prefix
        std::string keg_path = brew_prefix + "/opt/" + keg;
        if (fs::exists(keg_path)) {
            extra_cflags += " -I" + keg_path + "/include";
            extra_ldflags += " -L" + keg_path + "/lib";
            extra_pkgconfig += ":" + keg_path + "/lib/pkgconfig";
            if (keg == "e2fsprogs") {
                extra_libs += " -lext2fs -lcom_err -lblkid -luuid";
            }
        }
    }

    // Also add macman's own install prefix — unconditionally.
    // Use an absolute hardcoded fallback to ensure sudo environment resets do not wipe this string
    std::string safe_prefix = install_prefix;
    if (safe_prefix.empty() || safe_prefix == "/var/root/.macman" || safe_prefix.find("\n") != std::string::npos) {
        safe_prefix = macman::get_prefix();
    }

    extra_cflags += std::string(" -I") + safe_prefix + "/include";
    extra_cflags += std::string(" -I") + safe_prefix + "/include/ext2fs";
    extra_cflags += std::string(" -I") + safe_prefix + "/include/et";
    
    extra_ldflags += std::string(" -L") + safe_prefix + "/lib";
    extra_pkgconfig += std::string(":") + safe_prefix + "/lib/pkgconfig";

    // e2fsprogs keg-only libs needed when installed via macman prefix
    if (extra_libs.find("-lcom_err") == std::string::npos) {
        extra_libs += " -lext2fs -lcom_err -lblkid -luuid";
    }

    // Pre-build: Native C++ rewrite of .pc files to remove Homebrew bottle placeholders.
    // Avoids fragile BSD sed regex parsing and macOS permission lockouts.
    std::regex re_cellar("@@HOMEBREW_CELLAR@@/[^/]+/[^/]+");
    std::regex re_prefix("@@HOMEBREW_PREFIX@@");
    std::regex re_repo("@@HOMEBREW_REPOSITORY@@");
    std::regex re_usr_local("^prefix=/usr/local", std::regex::multiline);
    std::regex re_usr("^prefix=/usr$", std::regex::multiline);

    for (const auto& pc_dir_suffix : {"/lib/pkgconfig", "/share/pkgconfig"}) {
        std::string pc_dir = safe_prefix + pc_dir_suffix;
        if (!fs::exists(pc_dir)) continue;
        
        for (const auto& entry : fs::directory_iterator(pc_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".pc") {
                try {
                    // Unlock permissions for Homebrew 0444 files
                    fs::permissions(entry.path(), fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::add);
                    
                    std::ifstream in(entry.path());
                    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                    in.close();
                    
                    bool changed = false;
                    std::string new_content = std::regex_replace(content, re_cellar, safe_prefix);
                    if (new_content != content) { content = new_content; changed = true; }
                    
                    new_content = std::regex_replace(content, re_prefix, safe_prefix);
                    if (new_content != content) { content = new_content; changed = true; }
                    
                    new_content = std::regex_replace(content, re_repo, safe_prefix);
                    if (new_content != content) { content = new_content; changed = true; }
                    
                    new_content = std::regex_replace(content, re_usr_local, "prefix=" + safe_prefix);
                    if (new_content != content) { content = new_content; changed = true; }
                    
                    new_content = std::regex_replace(content, re_usr, "prefix=" + safe_prefix);
                    if (new_content != content) { content = new_content; changed = true; }

                    if (changed) {
                        std::ofstream out(entry.path(), std::ios::trunc);
                        out << content;
                        out.close();
                    }
                } catch (...) {}
            }
        }
    }

    // (Ext2fs include already forced above)

    std::string env_setup = "export PATH=\"" + brew_prefix + "/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH\" && "
                           "export CC=clang CXX=clang++ "
                           "CFLAGS=\"-O2 -I" + brew_prefix + "/include -I/usr/local/include" + extra_cflags + "\" "
                           "CXXFLAGS=\"-O2 -I" + brew_prefix + "/include -I/usr/local/include" + extra_cflags + "\" "
                           "LDFLAGS=\"-L" + brew_prefix + "/lib -L/usr/local/lib" + extra_ldflags + " -Wl,-headerpad_max_install_names\" "
                           "LIBS=\"" + extra_libs + "\" "
                           "PKG_CONFIG_PATH=\"" + brew_prefix + "/lib/pkgconfig:" + brew_prefix + "/share/pkgconfig:/usr/local/lib/pkgconfig" + extra_pkgconfig + "\" "
                           "EXPAT_LIBPATH=\"/usr/lib\" "
                           "PREFIX=\"" + install_prefix + "\" && ";

    // Detect if we need to force cross-compile mode for macOS Apple Silicon
    std::string extra_configure_flags = "";
    #if defined(__APPLE__) && (defined(__arm64__) || defined(__aarch64__))
        extra_configure_flags = " --host=arm-apple-darwin ";
    #endif

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

    std::string build_cmds = info.build_commands;
    // Replace ./configure with just configure to use our wrapper
    size_t pos_conf = 0;
    while ((pos_conf = build_cmds.find("./configure", pos_conf)) != std::string::npos) {
        build_cmds.replace(pos_conf, 11, "configure");
        pos_conf += 9;
    }

    // Extended macOS compatibility wrapper script
    // PATH and PKG_CONFIG are injected INSIDE the script so they survive posix_spawn
    std::string wrapper_script = build_dir_ + "/macman_makepkg_wrapper.sh";
    std::string script_content = R"BASH(#!/bin/bash
set -e

# ── PATH & Tool Setup (injected by macman) ────────────────────────────────────
export PATH=")BASH" + brew_prefix + R"BASH(/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH"
export PKG_CONFIG_EXECUTABLE=")BASH" + brew_prefix + R"BASH(/bin/pkg-config"

# ── macOS Compatibility Wrappers ─────────────────────────────────────────────

# Custom configure wrapper to inject --host if needed
configure() {
    local args=()
    local has_host=false
    for arg in "$@"; do
        if [[ "$arg" == --host=* ]]; then has_host=true; fi
        args+=("$arg")
    done
    if ! $has_host; then
        ./configure "${args[@]}")BASH" + extra_configure_flags + R"BASH(
    else
        ./configure "${args[@]}"
    fi
}

# GNU install -D → macOS install with mkdir -p
install() {
    local args=()
    local create_dirs=false
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -D*)
                create_dirs=true
                if [[ "$1" != "-D" ]]; then
                    # Handle combined like -Dm755
                    local rest="${1#-D}"
                    if [[ -n "$rest" ]]; then
                        set -- "-$rest" "${@:2}"
                        continue
                    fi
                fi
                ;;
            --mode)
                args+=("-m")
                ;;
            -t)
                args+=("-d")
                ;;
            -*)
                args+=("$1")
                ;;
            *)
                args+=("$1")
                ;;
        esac
        shift
    done
    if $create_dirs; then
        local cnt=${#args[@]}
        local target="${args[$(( cnt - 1 ))]}"
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

# ln --symbolic → -s, --force → -f, --relative/-r → compute relative path via python3
ln() {
    local args=()
    local do_relative=false
    for arg in "$@"; do
        if [[ "$arg" == "--symbolic" ]]; then args+=("-s")
        elif [[ "$arg" == "--force" ]]; then args+=("-f")
        elif [[ "$arg" == "--relative" || "$arg" == "-r" ]]; then do_relative=true
        else args+=("$arg")
        fi
    done
    if $do_relative && [[ ${#args[@]} -ge 2 ]]; then
        local cnt=${#args[@]}
        local target="${args[$(( cnt - 2 ))]}"
        local link="${args[$(( cnt - 1 ))]}"
        local rel
        rel=$(python3 -c "import os,sys; print(os.path.relpath(sys.argv[1], os.path.dirname(os.path.abspath(sys.argv[2]))))" "$target" "$link" 2>/dev/null)
        [[ -n "$rel" ]] && args[$(( cnt - 2 ))]="$rel"
    fi
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
        local cnt=${#args[@]}
        python3 -c "import os,sys; print(os.path.realpath(sys.argv[1]))" "${args[$(( cnt - 1 ))]}" 2>/dev/null || command readlink "${args[@]}"
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
export srcdir=")BASH" + work_dir + R"BASH("
export pkgdir=")BASH" + destdir + R"BASH("

# Source the original PKGBUILD so that all PKGBUILD-local variables (like _pkgname)
# are properly evaluated for the build environment.
source ")BASH" + build_dir_ + "/" + info.pkgname + "/PKGBUILD" + R"BASH("

# Override variables that makepkg normally injects
export pkgname=")BASH" + info.pkgname + R"BASH("
export pkgver=")BASH" + info.pkgver + R"BASH("

# ── Automatic Extraction ─────────────────────────────────────────────────────
cd "$srcdir"
for s in "${source[@]}"; do
    filename=$(echo "$s" | sed 's/::.*//')
    filename=$(basename "$filename")
    file_path="$srcdir/$filename"
    if [[ -f "$file_path" ]]; then
        case "$file_path" in
            *.tar.gz|*.tgz) tar -xzf "$file_path" -C "$srcdir" ;;
            *.tar.bz2|*.tbz2) tar -xjf "$file_path" -C "$srcdir" ;;
            *.tar.xz|*.txz) tar -xJf "$file_path" -C "$srcdir" ;;
            *.zip) unzip -q -o "$file_path" -d "$srcdir" ;;
        esac
    fi
done

cd "$srcdir"

build() {
    :
)BASH" + build_cmds + R"BASH(
}

package() {
    :
)BASH" + info.package_commands + R"BASH(
}

if type extract >/dev/null 2>&1; then
    cd "$srcdir"
    extract
fi

if type prepare >/dev/null 2>&1; then
    cd "$srcdir"
    prepare
fi

if type pkgver >/dev/null 2>&1; then
    cd "$srcdir"
    pkgver
fi

if type build >/dev/null 2>&1; then
    cd "$srcdir"
    build
fi

if type package >/dev/null 2>&1; then
    cd "$srcdir"
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
            // Use POSIX chmod() — fs::permissions() throws on macOS in some cases
            ::chmod(wrapper_script.c_str(), 0755);
        }

        std::string build_cmd = env_setup + wrapper_script;

        if (attempt == 0) {
            colors::print_substatus("Compiling with macOS native toolchain...");
        } else {
            colors::print_substatus("Retry " + std::to_string(attempt) + "/" + 
                                   std::to_string(MAX_BUILD_RETRIES - 1) + 
                                   ": Recompiling with applied fixes...");
        }

        ret = healing_engine_.run_build_capturing_output(build_cmd, build_output);
        
        if (ret == 0) {
            // Build succeeded!
            if (attempt > 0) {
                // colors::print_success("Build succeeded after self-healing (" +
                //                      std::to_string(attempt) + " fix round" +
                //                      (attempt > 1 ? "s" : "") + " applied)");
            }
            break;
        }
        // Build failed — try to fix
        if (attempt < MAX_BUILD_RETRIES - 1) {
            colors::print_warning("Build failed. Analyzing errors...");
            
            bool fixed = healing_engine_.analyze_and_fix_build(build_output, work_dir, src_dir, env_setup);
            
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
            if (entry.is_symlink() || entry.is_regular_file()) {
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
            
            // Deploy to target path using POSIX APIs
            fs::path target_dir = fs::path(remapped_path).parent_path();
            fs::create_directories(target_dir);

            if (fs::is_symlink(staged_path)) {
                // Read the symlink target
                std::string target = fs::read_symlink(staged_path).string();
                
                // If it's an absolute symlink pointing to /usr/..., remap it too!
                if (!target.empty() && target[0] == '/') {
                    target = remap_sys_path(target);
                }
                
                // Guard: skip self-referencing symlinks (target == link → ELOOP when accessed)
                if (target == remapped_path) continue;

                // Recreate symlink
                std::error_code ec;
                fs::remove(remapped_path, ec);
                fs::create_symlink(target, remapped_path, ec);
            } else {
                // Copy normal file
                int cp_ret = run_exec("/bin/cp", {"-f", staged_path, remapped_path});
                if (cp_ret != 0) {
                    cp_ret = run_exec("/bin/cp", {"-Rf", staged_path, remapped_path});
                }
                
                if (cp_ret != 0) {
                    colors::print_warning("Could not install: " + remapped_path + " (skipping)");
                    continue;
                }

                // Make binaries executable
                if (remapped_path.find("/bin/") != std::string::npos) {
                    ::chmod(remapped_path.c_str(), 0755);
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
            if (entry.is_symlink() || entry.is_regular_file()) {
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
        
        colors::print_warning("Linux kernel APIs detected (" + reason + ").");
        colors::print_substatus("macman Self-Healing Engine engaging compatibility wrapper...");
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

