// Arda Yiğit - Hazani
// aur_backend.cpp — Arch User Repository Source Builder Implementation [V1.2.0 Patch]

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
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include "core/checksum.hpp"

namespace fs = std::filesystem;

namespace macman {

AURBackend::AURBackend()
    : build_dir_(get_cache_dir() + "/builds"),
      healing_engine_(build_dir_) {
    fs::create_directories(build_dir_);
}

bool AURBackend::is_cache_valid(time_t timestamp) const {
    return (std::time(nullptr) - timestamp) < CACHE_TTL_SECONDS;
}

std::vector<Package> AURBackend::search(const std::string& query) {
    auto cache_it = search_cache_.find(query);
    if (cache_it != search_cache_.end() && is_cache_valid(cache_it->second.timestamp)) return cache_it->second.results;
    std::vector<Package> results;
    std::string url = std::string(AUR_RPC_BASE) + "?v=5&type=search&arg=" + query;
    auto response = http_.get_json(url);
    if (!response.success) return results;
    try {
        auto json = nlohmann::json::parse(response.body);
        if (json.contains("results") && json["results"].is_array()) {
            for (const auto& result : json["results"]) {
                results.push_back(aur_json_to_package(result));
                if (results.size() >= 25) break;
            }
        }
    } catch (...) {}
    search_cache_[query] = {results, std::time(nullptr)};
    return results;
}

std::optional<Package> AURBackend::get_info(const std::string& name) {
    auto cache_it = info_cache_.find(name);
    if (cache_it != info_cache_.end() && is_cache_valid(cache_it->second.timestamp)) return cache_it->second.pkg;
    std::string url = std::string(AUR_RPC_BASE) + "?v=5&type=info&arg[]=" + name;
    auto response = http_.get_json(url);
    if (!response.success) return std::nullopt;
    try {
        auto json = nlohmann::json::parse(response.body);
        if (json.contains("results") && json["results"].is_array() && !json["results"].empty()) {
            Package pkg = aur_json_to_package(json["results"][0]);
            info_cache_[name] = {pkg, std::time(nullptr)};
            return pkg;
        }
    } catch (...) {}
    return std::nullopt;
}

Package AURBackend::aur_json_to_package(const nlohmann::json& result) const {
    Package pkg;
    pkg.name = result.value("Name", "");
    pkg.version = result.value("Version", "");
    pkg.description = result.value("Description", "");
    pkg.homepage = result.value("URL", "");
    pkg.source = PackageSource::AUR;
    pkg.installed_size = result.value("InstalledSize", (size_t)0);

    // Extract dependencies from AUR JSON
    if (result.contains("Depends") && result["Depends"].is_array()) {
        for (const auto& dep : result["Depends"]) {
            pkg.dependencies.push_back(dep.get<std::string>());
        }
    }
    // Also consider MakeDepends for compilation
    if (result.contains("MakeDepends") && result["MakeDepends"].is_array()) {
        for (const auto& dep : result["MakeDepends"]) {
            pkg.dependencies.push_back(dep.get<std::string>());
        }
    }
    
    return pkg;
}

std::optional<PKGBUILDInfo> AURBackend::download_pkgbuild(const std::string& name) {
    std::string repo_url = "https://aur.archlinux.org/" + name + ".git";
    std::string extract_dir = build_dir_ + "/" + name;
    
    if (fs::exists(extract_dir + "/PKGBUILD")) {
        return parse_pkgbuild(extract_dir + "/PKGBUILD");
    }

    colors::print_substatus("Cloning " + name + "...");
    if (fs::exists(extract_dir)) fs::remove_all(extract_dir);
    // Suppress git output with > /dev/null
    if (run_exec("/usr/bin/git", {"clone", "--depth", "1", "--quiet", repo_url, extract_dir}, false) != 0) return std::nullopt;
    return parse_pkgbuild(extract_dir + "/PKGBUILD");
}

PKGBUILDInfo AURBackend::parse_pkgbuild(const std::string& path) const {
    PKGBUILDInfo info;
    std::string dumper = "#!/bin/bash\nsource \"$1\" 2>/dev/null\necho \"PKGNAME\"\necho \"${pkgname}\"\necho \"PKGVER\"\necho \"${pkgver}\"\necho \"SOURCE\"\nfor s in \"${source[@]}\"; do echo \"$s\"; done\necho \"DEPENDS\"\nfor d in \"${depends[@]}\"; do echo \"$d\"; done\necho \"MAKEDEPENDS\"\nfor d in \"${makedepends[@]}\"; do echo \"$d\"; done\n";
    std::string script = build_dir_ + "/dumper.sh";
    { std::ofstream out(script); out << dumper; }
    ::chmod(script.c_str(), 0755);
    FILE* p = popen(("bash " + script + " '" + path + "'").c_str(), "r");
    if (p) {
        char b[1024]; std::string state;
        while (fgets(b, sizeof(b), p)) {
            std::string l = b; if (!l.empty() && l.back() == '\n') l.pop_back();
            if (l == "PKGNAME" || l == "PKGVER" || l == "SOURCE" || l == "DEPENDS" || l == "MAKEDEPENDS") { state = l; continue; }
            if (state == "PKGNAME") info.pkgname = l;
            else if (state == "PKGVER") info.pkgver = l;
            else if (state == "SOURCE") info.source.push_back(l);
            else if (state == "DEPENDS") info.depends.push_back(l);
            else if (state == "MAKEDEPENDS") info.makedepends.push_back(l);
        }
        pclose(p);
    }
    fs::remove(script);
    std::ifstream f(path); std::string c((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto get_fn = [&](std::string n) {
        size_t s = c.find(n + "()"); if (s == std::string::npos) s = c.find(n + " ()");
        if (s == std::string::npos) return std::string("");
        size_t b = c.find('{', s); if (b == std::string::npos) return std::string("");
        int d = 0; for (size_t i = b; i < c.size(); i++) {
            if (c[i] == '{') d++; if (c[i] == '}') d--;
            if (d == 0) return c.substr(b + 1, i - b - 1);
        }
        return std::string("");
    };
    info.build_commands = get_fn("build");
    info.package_commands = get_fn("package");
    return info;
}

CompatLevel AURBackend::check_macos_compatibility(const PKGBUILDInfo& info) const { return CompatLevel::COMPATIBLE; }
std::string AURBackend::get_incompatibility_reason(const PKGBUILDInfo& info) const { return ""; }

bool AURBackend::download_sources(const PKGBUILDInfo& info, const std::string& work_dir, const std::string& repo_dir) {
    for (auto s : info.source) {
        std::string filename;
        std::string url;

        size_t dpos = s.find("::");
        if (dpos != std::string::npos) {
            filename = s.substr(0, dpos);
            url = s.substr(dpos + 2);
        } else {
            url = s;
            filename = url.substr(url.rfind('/') + 1);
        }

        if (url.find("git+") == 0 || url.find("git://") == 0) {
            std::string real_url = url;
            if (url.find("git+") == 0) real_url = url.substr(4);
            std::string target_dir = filename;
            if (target_dir.empty()) target_dir = info.pkgname;

            colors::print_substatus("Cloning " + target_dir + "...");
            if (run_exec("/usr/bin/git", {"clone", "--depth", "1", "--quiet", real_url, target_dir}, false, work_dir) != 0) return false;
        } else if (url.find("://") != std::string::npos) {
            colors::print_substatus("Fetching " + filename + "...");
            if (run_exec("/usr/bin/curl", {"-L", "-s", "-o", filename, url}, false, work_dir) != 0) return false;
        } else {
            std::error_code ec;
            fs::copy(repo_dir + "/" + url, work_dir + "/" + filename, fs::copy_options::overwrite_existing, ec);
        }

        // Extract silently
        if (filename.find(".tar.") != std::string::npos || filename.find(".tgz") != std::string::npos) {
            run_exec("/usr/bin/tar", {"-xf", filename}, false, work_dir);
        } else if (filename.find(".zip") != std::string::npos) {
            run_exec("/usr/bin/unzip", {"-q", filename}, false, work_dir);
        }
    }
    return true;
}

bool AURBackend::compile_source(const PKGBUILDInfo& info, const std::string& work_dir, const std::string& prefix, std::vector<std::string>& files, std::string& ver) {
    std::string brew = fs::exists("/opt/homebrew/bin") ? "/opt/homebrew" : "/usr/local";
    std::string m_root = get_prefix();
    std::string extra_cflags = "-I" + m_root + "/include";
    std::string extra_ldflags = "-L" + m_root + "/lib";
    
    // Optimize for M4: Use N-2 cores for compilation
    int cores = std::thread::hardware_concurrency();
    int jobs = std::max(1, cores - 2);
    std::string makeflags = "-j" + std::to_string(jobs);

    std::string dest = build_dir_ + "/dest-" + info.pkgname;
    fs::create_directories(dest + "/usr/bin");
    fs::create_directories(dest + "/usr/local/bin");
    fs::create_directories(dest + "/usr/share/man/man1");

    // Create a patch shim to force non-interactive mode
    std::string shim_dir = build_dir_ + "/shims";
    fs::create_directories(shim_dir);
    {
        std::ofstream shim(shim_dir + "/patch");
        shim << "#!/bin/bash\n"
             << "exec /usr/bin/patch --batch --forward --silent --force \"$@\" < /dev/null\n";
    }
    ::chmod((shim_dir + "/patch").c_str(), 0755);

    // Create an advanced install shim to use ginstall (from coreutils) if available, fixing macOS install -t / -D issues
    {
        std::ofstream shim(shim_dir + "/install");
        shim << "#!/bin/bash\n"
             << "if command -v ginstall >/dev/null 2>&1; then\n"
             << "    TARGET=\"\"\n"
             << "    for (( i=1; i<=$#; i++ )); do\n"
             << "        if [[ \"${!i}\" == \"-t\" ]]; then\n"
             << "            j=$((i+1))\n"
             << "            TARGET=\"${!j}\"\n"
             << "        elif [[ \"${!i}\" == \"--target-directory=\"* ]]; then\n"
             << "            TARGET=\"${!i#*=}\"\n"
             << "        elif [[ \"${!i}\" == \"-d\" ]]; then\n"
             << "            exec ginstall \"$@\"\n"
             << "        fi\n"
             << "    done\n"
             << "    if [ -n \"$TARGET\" ]; then\n"
             << "        mkdir -p \"$TARGET\" 2>/dev/null\n"
             << "    else\n"
             << "        LAST=\"${@: -1}\"\n"
             << "        mkdir -p \"$(dirname \"$LAST\")\" 2>/dev/null\n"
             << "    fi\n"
             << "    exec ginstall \"$@\"\n"
             << "else\n"
             << "    exec /usr/bin/install \"$@\"\n"
             << "fi\n";
    }
    ::chmod((shim_dir + "/install").c_str(), 0755);

    for (int attempt = 0; attempt < MAX_BUILD_RETRIES; attempt++) {
        std::string env_setup = "export PATH=\"" + shim_dir + ":" + brew + "/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH\"; "
                               "export CC=clang; export CXX=clang++; "
                               "export PATCH_GET=0; export PATCH_INTERACTIVE=0; "
                               "export DEBIAN_FRONTEND=noninteractive; export NONINTERACTIVE=1; "
                               "export HOMEBREW_NO_INTERACTIVE=1; "
                               "export MAKEFLAGS=\"" + makeflags + "\"; "
                               "export CFLAGS=\"-O2 " + extra_cflags + "\"; "
                               "export CXXFLAGS=\"-O2 " + extra_cflags + "\"; "
                               "export LDFLAGS=\"-L" + brew + "/lib " + extra_ldflags + "\"; ";

        std::string wrap = build_dir_ + "/wrap.sh";
        std::string sc = "#!/bin/bash\n"
                         "export HOMEBREW_NO_INTERACTIVE=1\n"
                         "exec < /dev/null\n"
                         "set -e\n"
                         "export pkgname=\"" + info.pkgname + "\"\n"
                         "export pkgver=\"" + info.pkgver + "\"\n"
                         "export srcdir=\"" + work_dir + "\"\n"
                         "export pkgdir=\"" + dest + "\"\n"
                         "cd \"$srcdir\"\n"
                         "source \"" + build_dir_ + "/" + info.pkgname + "/PKGBUILD\"\n"
                         "build() {\n :; " + info.build_commands + "\n}\n"
                         "package() {\n :; " + info.package_commands + "\n}\n"
                         "if [ " + std::to_string(attempt) + " -gt 0 ] && [ -f Makefile ]; then\n"
                         "  make " + makeflags + " < /dev/null\n"
                         "else\n"
                         "  ( build ) < /dev/null\n"
                         "fi\n"
                         "( package ) < /dev/null\n";
        { std::ofstream out(wrap); out << sc; } ::chmod(wrap.c_str(), 0755);


        if (attempt == 0) colors::print_substatus("Compiling " + info.pkgname + "...");
        else {
            colors::print_substatus("Retry " + std::to_string(attempt) + ": Applying Self-Healing...");
            std::cout.flush();
        }

        // Forcefully ensure our own stdin is not interfering before we spawn the build
        int null_fd = ::open("/dev/null", O_RDONLY);
        if (null_fd != -1) {
            ::dup2(null_fd, STDIN_FILENO);
            ::close(null_fd);
        }

        std::string build_output;
        int ret = healing_engine_.run_build_capturing_output(env_setup + " bash " + wrap + " </dev/null", build_output);
        if (ret == 0) break;

        if (attempt < MAX_BUILD_RETRIES - 1) {
            colors::print_warning("Build failed. Analyzing errors...");
            if (!healing_engine_.analyze_and_fix_build(build_output, work_dir, work_dir, extra_cflags, extra_ldflags, info.pkgname)) {
                colors::print_error("No automatic fix could be applied.");
                return false;
            }
        } else return false;
    }

    for (const auto& e : fs::recursive_directory_iterator(dest)) {
        if (!e.is_regular_file()) continue;
        std::string r = e.path().string().substr(dest.length());
        std::string d = prefix + r;
        fs::create_directories(fs::path(d).parent_path());
        run_exec("/bin/cp", {"-f", e.path().string(), d});
        files.push_back(d);
    }
    return true;
}

bool AURBackend::build_and_install(const std::string& name, const std::string& prefix, std::vector<std::string>& files, std::string& ver) {
    auto p = download_pkgbuild(name); if (!p) return false;
    std::string repo_dir = build_dir_ + "/" + name;
    std::string w = build_dir_ + "/" + name + "-src";
    fs::create_directories(w);
    if (!download_sources(*p, w, repo_dir)) return false;
    return compile_source(*p, w, prefix, files, ver);
}

bool AURBackend::has_package(const std::string& name) { return get_info(name).has_value(); }

} // namespace macman
