// Arda Yiğit - Hazani
// installer.cpp [V1.2.0 Patch]
// Self-Healing Compilation Engine added in V1.2.0:
//   run_capturing_output  — posix_spawn stderr capture
//   analyze_and_fix_compile_errors — regex-driven Darwin patch engine
//   patch_build_flags     — Makefile/CMake flag surgery
//   patch_cmake_remove_required — strips REQUIRED from find_package
//   build_with_healing    — retry loop with progressive fix accumulation

#include "core/installer.hpp"
#include "backend/homebrew_backend.hpp"
#include "backend/aur_backend.hpp"
#include "net/downloader.hpp"
#include "macman.hpp"
#include "cli/colors.hpp"
#include "core/process.hpp"
#include <iostream>
#include <csignal>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstdlib>
#include <vector>
#include <map>
#include <set>
#include <regex>
#include <algorithm>
#include <spawn.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#ifdef __APPLE__
#  include <crt_externs.h>
#  define MACMAN_ENVIRON (*_NSGetEnviron())
#else
extern char** environ;
#  define MACMAN_ENVIRON environ
#endif

#include "core/checksum.hpp"

namespace fs = std::filesystem;

namespace macman {

Installer::Installer(Database& db) : db_(db) {}

void Installer::fix_macho_rpaths(const std::string& deploy_dir) const {
    // We traverse /bin and /lib to fix any broken dynamic library linkage
    std::string prefix = get_prefix();
    std::vector<std::string> target_dirs = {deploy_dir + "/bin", deploy_dir + "/lib"};

    bool any_patched = false;

    for (const auto& dir : target_dirs) {
        if (!fs::exists(dir)) continue;

        for (const auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_regular_file() && !entry.is_symlink()) continue;
            if (entry.is_symlink()) continue;

            std::string file = entry.path().string();
            std::string filename = entry.path().filename().string();
            bool is_dylib = (entry.path().extension() == ".dylib");
            bool is_bin = (dir == deploy_dir + "/bin");

            if (!is_dylib && !is_bin) continue;

            if (!any_patched) {
                colors::print_substatus("Patching Mach-O binaries and RPATHs...");
                any_patched = true;
            }

            // Ensure write access for mach-o tools
            ::chmod(file.c_str(), 0755);

            std::string rpath_out, id_out;
            run_capturing_output("/usr/bin/install_name_tool -add_rpath '" + prefix + "/lib' '" + file + "' 2>/dev/null", rpath_out);

            if (is_dylib) {
                run_capturing_output("/usr/bin/install_name_tool -id '" + prefix + "/lib/" + filename + "' '" + file + "' 2>/dev/null", id_out);
            }

            std::string otool_out;
            if (run_capturing_output("/usr/bin/otool -L '" + file + "' 2>/dev/null", otool_out) == 0) {
                std::istringstream stream(otool_out);
                std::string line;
                while (std::getline(stream, line)) {
                    size_t first_slash = line.find_first_not_of(" \t");
                    if (first_slash == std::string::npos) continue;
                    size_t space = line.find(' ', first_slash);
                    if (space == std::string::npos) continue;

                    std::string dylib_path = line.substr(first_slash, space - first_slash);

                    if (dylib_path.find("@@HOMEBREW") != std::string::npos ||
                        dylib_path.find("/opt/homebrew/") != std::string::npos ||
                        dylib_path.find("/usr/local/opt/") != std::string::npos ||
                        dylib_path.find("Cellar/") != std::string::npos) {
                        
                        std::string target_file_name = fs::path(dylib_path).filename().string();
                        std::string new_path = prefix + "/lib/" + target_file_name;
                        
                        std::string change_out;
                        run_capturing_output("/usr/bin/install_name_tool -change '" + dylib_path + "' '" + new_path + "' '" + file + "' 2>/dev/null", change_out);
                    }
                }
            }

            // Re-sign the binary after modifying load commands. This is mandatory on Apple Silicon.
            std::string codesign_out;
            run_capturing_output("/usr/bin/codesign --sign - --force --preserve-metadata=identifier,entitlements,flags '" + file + "' 2>/dev/null", codesign_out);
        }
    }
}

bool Installer::atomic_commit(const std::string& stage_dir, const std::string& final_dir) const {
    fs::create_directories(final_dir); // Ensure prefix exists

    // We merge stage_dir contents into final_dir atomically
    try {
        auto options = fs::directory_options::none; // DO NOT follow symlinks!
        for (auto it = fs::recursive_directory_iterator(stage_dir, options); 
             it != fs::recursive_directory_iterator(); ++it) {
                 
            const auto& entry = *it;
            // Prevent stat() on the target of a broken/cyclic symlink!
            if (entry.is_symlink()) {
                // Do not throw on directory checking
            } else if (entry.is_directory()) {
                continue;
            }

            auto rel_path = fs::relative(entry.path(), stage_dir);
            auto target_path = fs::path(final_dir) / rel_path;

            // Skip symlinks that would be self-referential after placement (e.g. share/terminfo → ../share/terminfo)
            if (entry.is_symlink()) {
                fs::path sym_target = fs::read_symlink(entry.path());
                if (sym_target.is_relative()) {
                    auto resolved = (target_path.parent_path() / sym_target).lexically_normal();
                    if (resolved == target_path.lexically_normal()) continue;
                }
            }

            {
                std::error_code ec;
                fs::create_directories(target_path.parent_path(), ec);
                if (ec == std::errc::too_many_symbolic_link_levels) {
                    // Find the circular symlink component and replace it with a real dir
                    fs::path cur;
                    for (const auto& part : target_path.parent_path()) {
                        cur /= part;
                        std::error_code ec2;
                        fs::status(cur, ec2);
                        if (ec2 == std::errc::too_many_symbolic_link_levels) {
                            fs::remove(cur, ec2);
                            fs::create_directory(cur, ec2);
                            break;
                        }
                    }
                    fs::create_directories(target_path.parent_path(), ec);
                }
                if (ec) continue;
            }

            // Overwrite existing without resolving symlink targets
            std::error_code ec;
            if (fs::exists(fs::symlink_status(target_path))) {
                fs::remove(target_path, ec);
            }
            
            fs::rename(entry.path(), target_path, ec);
            if (ec) {
                // Move fallback block via cp -a and rm in case of cross-device links
                if (run_exec("/bin/cp", {"-a", entry.path().string(), target_path.string()}) == 0) {
                    std::error_code rm_ec;
                    fs::remove(entry.path(), rm_ec);
                }
            }
        }
        // Cleanup stage
        fs::remove_all(stage_dir);
        return true;
    } catch (const std::exception& e) {
        colors::print_error(std::string("Atomic commit failed: ") + e.what());
        return false;
    }
}

bool Installer::link_to_prefix(const std::string& pkg_dir, std::vector<std::string>& installed_files) const {
    std::string prefix = get_prefix();
    
    try {
        for (auto const& entry : fs::recursive_directory_iterator(pkg_dir)) {
            if (entry.is_directory()) continue;

            auto rel_path = fs::relative(entry.path(), pkg_dir);
            std::string rel_str = rel_path.string();
            
            // Strip "usr/" from the beginning so binaries go to bin/ instead of usr/bin/
            if (rel_str.find("usr/") == 0) {
                rel_str = rel_str.substr(4);
            }
            
            auto target_path = fs::path(prefix) / rel_str;

            // Skip common metadata files (INSTALL_RECEIPT, etc)
            if (rel_path.string().find("INSTALL_RECEIPT") != std::string::npos) continue;

            fs::create_directories(target_path.parent_path());
            
            if (fs::exists(target_path) || fs::is_symlink(target_path)) {
                fs::remove(target_path);
            }

            fs::create_symlink(entry.path(), target_path);
            installed_files.push_back(target_path.string());
        }
        return true;
    } catch (const std::exception& e) {
        colors::print_error("Failed to link package: " + std::string(e.what()));
        return false;
    }
}

void Installer::record_hashes(const std::string& pkg_dir, std::map<std::string, std::string>& hashes) const {
    try {
        for (auto const& entry : fs::recursive_directory_iterator(pkg_dir)) {
            if (entry.is_regular_file()) {
                std::string path = entry.path().string();
                std::string hash = Checksum::compute_sha256(path);
                if (!hash.empty()) {
                    hashes[path] = hash;
                }
            }
        }
    } catch (...) {}
}

bool Installer::install_package(const Package& pkg, const std::string& reason) {
    if (pkg.version == "macOS-system-stub") {
        colors::print_substatus("Using macOS system provider for " + pkg.name);
        return true;
    }
    Package installed = pkg;
    installed.install_reason = reason;

    // Set install date
    auto now = std::time(nullptr);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
    installed.install_date = buf;
    
    // Final opt directory: ~/.macman/opt/<name>
    std::string opt_dir = get_prefix() + "/opt/" + pkg.name;
    if (fs::exists(opt_dir)) fs::remove_all(opt_dir);
    fs::create_directories(opt_dir);

    // Stage directory
    std::string stage_dir = get_cache_dir() + "/stage_" + pkg.name;
    if (fs::exists(stage_dir)) fs::remove_all(stage_dir);
    fs::create_directories(stage_dir);

    bool build_success = false;
    std::vector<std::string> stage_files;
    
    if (pkg.source == PackageSource::AUR) {
        AURBackend aur;
        std::string actual_version;
        build_success = aur.build_and_install(pkg.name, opt_dir, stage_files, actual_version);
        if (build_success) {
            if (!actual_version.empty() && actual_version != installed.version) {
                installed.version = actual_version;
            }
            fix_macho_rpaths(opt_dir);
            colors::print_substatus("Calculating file hashes for verification...");
            record_hashes(opt_dir, installed.file_hashes);
            build_success = link_to_prefix(opt_dir, installed.installed_files);
        }
    } else {
        // Brew Bottle branch
        if (pkg.url.empty()) {
            colors::print_error("No URL for " + pkg.name);
            return false;
        }

        std::string tarball_path = get_cache_dir() + "/" + pkg.name + "-" + pkg.version + ".tar.gz";

        if (!fs::exists(tarball_path) || fs::file_size(tarball_path) < 100) {
            Downloader dl;
            DownloadTask task;
            task.label = pkg.name;
            task.expected_size = pkg.download_size;
            task.url = pkg.url;
            task.output_path = tarball_path;

            auto result = dl.download(task);
            if (!result.success) {
                colors::print_error(result.error);
                return false;
            }
        }
        
        // Silent checksum
        if (!pkg.sha256.empty()) {
            if (!Checksum::verify_sha256(tarball_path, pkg.sha256)) {
                colors::print_error("SHA-256 mismatch");
                return false;
            }
        }

        HomebrewBackend brew;
        build_success = brew.install_bottle(tarball_path, stage_dir, stage_files);
        if (build_success) {
            fix_macho_rpaths(stage_dir);
            if (atomic_commit(stage_dir, opt_dir)) {
                colors::print_substatus("Calculating file hashes for verification...");
                record_hashes(opt_dir, installed.file_hashes);
                build_success = link_to_prefix(opt_dir, installed.installed_files);
            } else {
                build_success = false;
            }
        }
    }
    
    // Cleanup stage if failed
    if (fs::exists(stage_dir)) fs::remove_all(stage_dir);

    if (build_success) {
        // Record both the symlinks AND the opt directory itself
        // This ensures the entire opt folder is removed on uninstall or rollback
        installed.installed_files.push_back(opt_dir);
        if (!db_.add_package(installed)) {
            colors::print_error("Failed to record package in database: " + pkg.name);
            // Optional: cleanup opt_dir? For now just return false.
            return false;
        }
        return true;
    } else {
        // Cleanup if installation failed or was aborted
        if (fs::exists(opt_dir)) fs::remove_all(opt_dir);
    }
    return false;
}



// ─────────────────────────────────────────────────────────────────────────────
// Self-Healing Compilation Engine
// ─────────────────────────────────────────────────────────────────────────────

int Installer::run_capturing_output(const std::string& cmd, std::string& output) const {
    output.clear();
    std::string log_path = get_cache_dir() + "/compile_heal.log";

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    
    // Create a dummy pipe to use as a non-interactive stdin
    int pipefds[2];
    if (pipe(pipefds) == 0) {
        posix_spawn_file_actions_adddup2(&fa, pipefds[0], STDIN_FILENO);
        posix_spawn_file_actions_addclose(&fa, pipefds[0]);
        posix_spawn_file_actions_addclose(&fa, pipefds[1]);
    } else {
        posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    }

    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, log_path.c_str(),
                                     O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, STDOUT_FILENO, STDERR_FILENO);

    pid_t pid;
    const char* argv[] = {"sh", "-c", cmd.c_str(), nullptr};
    int status = posix_spawn(&pid, "/bin/sh", &fa, nullptr,
                              (char* const*)argv, MACMAN_ENVIRON);
    posix_spawn_file_actions_destroy(&fa);

    if (pipefds[0] != -1) {
        close(pipefds[0]);
        close(pipefds[1]);
    }

    if (status == 0) {
        int ws;
        if (waitpid(pid, &ws, 0) != -1) {
            status = WIFEXITED(ws) ? WEXITSTATUS(ws) : -1;
        } else {
            status = -1;
        }
    } else {
        status = -1;
    }

    try {
        std::ifstream f(log_path);
        if (f.is_open())
            output.assign(std::istreambuf_iterator<char>(f),
                          std::istreambuf_iterator<char>());
    } catch (...) {}

    return status;
}

// Removes/replaces old_flag in every Makefile, CMakeLists.txt, .mk,
// configure, and meson.build under src_dir.
void Installer::patch_build_flags(const std::string& src_dir,
                                   const std::string& old_flag,
                                   const std::string& new_flag) const {
    static const std::vector<std::string> targets = {
        "Makefile", "CMakeLists.txt", "configure", "meson.build"
    };

    try {
        for (const auto& entry : fs::recursive_directory_iterator(src_dir)) {
            if (!entry.is_regular_file()) continue;
            std::string fname = entry.path().filename().string();
            bool is_target = false;
            for (const auto& t : targets) {
                if (fname == t || fname.rfind(".mk") == fname.size() - 3) {
                    is_target = true; break;
                }
            }
            if (!is_target) continue;

            std::string content;
            {
                std::ifstream f(entry.path());
                content.assign(std::istreambuf_iterator<char>(f),
                               std::istreambuf_iterator<char>());
            }
            if (content.find(old_flag) == std::string::npos) continue;

            size_t pos;
            while ((pos = content.find(old_flag)) != std::string::npos)
                content.replace(pos, old_flag.size(), new_flag);

            std::ofstream f(entry.path());
            f << content;
        }
    } catch (...) {}
}

// Strips REQUIRED from find_package/find_program calls for pkg_name,
// and comments out lines that would be fatal_error for that package.
void Installer::patch_cmake_remove_required(const std::string& src_dir,
                                             const std::string& pkg_name) const {
    std::string lower_pkg = pkg_name;
    std::transform(lower_pkg.begin(), lower_pkg.end(), lower_pkg.begin(), ::tolower);

    try {
        for (const auto& entry : fs::recursive_directory_iterator(src_dir)) {
            if (!entry.is_regular_file()) continue;
            std::string fname = entry.path().filename().string();
            bool is_cmake = (fname == "CMakeLists.txt" ||
                             (fname.size() > 6 &&
                              fname.substr(fname.size() - 6) == ".cmake"));
            if (!is_cmake) continue;

            std::string content;
            {
                std::ifstream f(entry.path());
                content.assign(std::istreambuf_iterator<char>(f),
                               std::istreambuf_iterator<char>());
            }

            std::string lower_content = content;
            std::transform(lower_content.begin(), lower_content.end(),
                           lower_content.begin(), ::tolower);
            if (lower_content.find(lower_pkg) == std::string::npos) continue;

            std::istringstream ss(content);
            std::string new_content;
            std::string line;
            bool modified = false;

            while (std::getline(ss, line)) {
                std::string ll = line;
                std::transform(ll.begin(), ll.end(), ll.begin(), ::tolower);

                bool mentions_pkg = ll.find(lower_pkg) != std::string::npos;

                if (mentions_pkg && ll.find("fatal_error") != std::string::npos) {
                    new_content += "# [macman patched] " + line + "\n";
                    modified = true;
                    continue;
                }

                if (mentions_pkg &&
                    (ll.find("find_package") != std::string::npos ||
                     ll.find("find_program") != std::string::npos)) {
                    // Remove REQUIRED keyword
                    size_t rpos = line.find("REQUIRED");
                    if (rpos != std::string::npos) {
                        line.erase(rpos, 8);
                        modified = true;
                    }
                }

                new_content += line + "\n";
            }

            if (modified) {
                std::ofstream f(entry.path());
                f << new_content;
            }
        }
    } catch (...) {}
}

bool Installer::analyze_and_fix_compile_errors(const std::string& log,
                                                const std::string& src_dir,
                                                const std::string& compat_dir,
                                                std::string& extra_cflags) const {
    bool any_fix = false;

    // ── Load / init compat header ────────────────────────────────────────────
    std::string compat_h = compat_dir + "/macman_compat.h";
    std::string compat_content;
    if (fs::exists(compat_h)) {
        std::ifstream f(compat_h);
        compat_content.assign(std::istreambuf_iterator<char>(f),
                              std::istreambuf_iterator<char>());
    } else {
        compat_content = "#pragma once\n/* macman Darwin self-healing compat header */\n\n";
    }

    // Appends code only if it hasn't been added yet (fingerprint = first 32 chars).
    auto append_if_new = [&](const std::string& code) -> bool {
        std::string fp = code.substr(0, std::min((size_t)32, code.size()));
        if (compat_content.find(fp) != std::string::npos) return false;
        compat_content += code + "\n\n";
        return true;
    };

    // Filter log to only lines containing keywords to speed up regex matching
    std::string error_log;
    {
        std::istringstream ss(log);
        std::string line;
        while (std::getline(ss, line)) {
            std::string lower_line = line;
            std::transform(lower_line.begin(), lower_line.end(), lower_line.begin(), ::tolower);
            if (lower_line.find("error") != std::string::npos || 
                lower_line.find("not found") != std::string::npos ||
                lower_line.find("undeclared") != std::string::npos ||
                lower_line.find("referenced from") != std::string::npos ||
                lower_line.find("unknown") != std::string::npos ||
                lower_line.find("unsupported") != std::string::npos ||
                lower_line.find("undefined") != std::string::npos) {
                error_log += line + "\n";
            }
        }
    }

    // ── [1] Missing headers ──────────────────────────────────────────────────
    // Clang: fatal error: 'X' file not found
    // GCC:   fatal error: X: No such file or directory
    {
        std::regex re1(R"(fatal error: '([^']+\.h(?:pp)?)' file not found)");
        std::regex re2(R"(fatal error: ([^\s:]+\.h(?:pp)?): No such file or directory)");

        std::set<std::string> missing;
        for (auto& re : {re1, re2})
            for (std::sregex_iterator it(error_log.begin(), error_log.end(), re);
                 it != std::sregex_iterator{}; ++it)
                missing.insert((*it)[1].str());

        // Darwin-specific header remappings: map Linux header → Darwin snippet
        static const std::map<std::string, std::string> hdr_fixes = {
            {"malloc.h",
             "#ifdef __APPLE__\n"
             "#include <malloc/malloc.h>\n"
             "#define malloc_usable_size(p) malloc_size(p)\n"
             "#endif"},
            {"endian.h",
             "#ifdef __APPLE__\n"
             "#include <machine/endian.h>\n"
             "#ifndef __BYTE_ORDER\n"
             "#  define __BYTE_ORDER    BYTE_ORDER\n"
             "#  define __BIG_ENDIAN    BIG_ENDIAN\n"
             "#  define __LITTLE_ENDIAN LITTLE_ENDIAN\n"
             "#  define __PDP_ENDIAN    PDP_ENDIAN\n"
             "#endif\n"
             "#endif"},
            {"byteswap.h",
             "#ifdef __APPLE__\n"
             "#ifndef bswap_16\n"
             "#  define bswap_16(x) __builtin_bswap16(x)\n"
             "#  define bswap_32(x) __builtin_bswap32(x)\n"
             "#  define bswap_64(x) __builtin_bswap64(x)\n"
             "#endif\n"
             "#endif"},
            {"pty.h",
             "#ifdef __APPLE__\n"
             "#include <util.h>\n"
             "#endif"},
            {"bsd/string.h",
             "#ifdef __APPLE__\n"
             "#include <string.h>\n"
             "#endif"},
            {"error.h",
             "#ifdef __APPLE__\n"
             "#include <stdio.h>\n"
             "#include <stdlib.h>\n"
             "#include <string.h>\n"
             "#include <errno.h>\n"
             "#include <stdarg.h>\n"
             "static inline void error(int status, int errnum,\n"
             "                         const char* fmt, ...) {\n"
             "    va_list ap; va_start(ap, fmt);\n"
             "    vfprintf(stderr, fmt, ap);\n"
             "    va_end(ap);\n"
             "    if (errnum) fprintf(stderr, \": %s\", strerror(errnum));\n"
             "    fputc('\\n', stderr);\n"
             "    if (status) exit(status);\n"
             "}\n"
             "#endif"},
            {"sys/random.h",
             "#ifdef __APPLE__\n"
             "#include <sys/types.h>\n"
             "extern int getentropy(void* buf, size_t len); /* available since macOS 10.12 */\n"
             "static inline ssize_t getrandom(void* buf, size_t n, unsigned f) {\n"
             "    (void)f;\n"
             "    return getentropy(buf, n) == 0 ? (ssize_t)n : -1;\n"
             "}\n"
             "#endif"},
            {"threads.h",
             "#ifdef __APPLE__\n"
             "#include <pthread.h>\n"
             "#include <stdint.h>\n"
             "typedef pthread_t thrd_t;\n"
             "typedef int (*thrd_start_t)(void*);\n"
             "typedef pthread_mutex_t mtx_t;\n"
             "#define thrd_success 0\n"
             "#define thrd_error   1\n"
             "#define mtx_plain    0\n"
             "static inline int thrd_create(thrd_t* t, thrd_start_t f, void* a) {\n"
             "    return pthread_create(t,NULL,(void*(*)(void*))f,a) ? thrd_error : thrd_success;\n"
             "}\n"
             "static inline int thrd_join(thrd_t t, int* r) {\n"
             "    void* v; pthread_join(t, &v);\n"
             "    if (r) *r = (int)(intptr_t)v; return thrd_success;\n"
             "}\n"
             "static inline int mtx_init(mtx_t* m, int tp) {\n"
             "    (void)tp; return pthread_mutex_init(m,NULL) ? thrd_error : thrd_success;\n"
             "}\n"
             "static inline int mtx_lock(mtx_t* m)   { return pthread_mutex_lock(m)   ? thrd_error : thrd_success; }\n"
             "static inline int mtx_unlock(mtx_t* m) { return pthread_mutex_unlock(m) ? thrd_error : thrd_success; }\n"
             "static inline void mtx_destroy(mtx_t* m) { pthread_mutex_destroy(m); }\n"
             "#endif"},
            {"sys/sysinfo.h",
             "#ifdef __APPLE__\n"
             "#include <sys/types.h>\n"
             "#include <sys/sysctl.h>\n"
             "struct sysinfo {\n"
             "    long uptime; unsigned long loads[3];\n"
             "    unsigned long totalram, freeram, sharedram, bufferram;\n"
             "    unsigned long totalswap, freeswap;\n"
             "    unsigned short procs; unsigned long totalhigh, freehigh;\n"
             "    unsigned int mem_unit;\n"
             "};\n"
             "static inline int sysinfo(struct sysinfo* s) {\n"
             "    if (!s) return -1;\n"
             "    *s = (struct sysinfo){};\n"
             "    size_t sz = sizeof(s->totalram);\n"
             "    sysctlbyname(\"hw.memsize\", &s->totalram, &sz, NULL, 0);\n"
             "    s->mem_unit = 1; return 0;\n"
             "}\n"
             "#endif"},
        };

        for (const auto& hdr : missing) {
            auto it = hdr_fixes.find(hdr);
            if (it != hdr_fixes.end()) {
// Silent fix
                if (append_if_new(it->second)) any_fix = true;
            } else {
                // Generic stub: covers linux/*, sys/epoll.h, sys/inotify.h, …
                fs::path stub = fs::path(compat_dir) / hdr;
                fs::create_directories(stub.parent_path());
                if (!fs::exists(stub)) {
                    std::ofstream sf(stub.string());
                    sf << "/* macman auto-stub: <" << hdr
                       << "> not available on Darwin */\n";
                    colors::print_substatus("Self-healing: stub created for <" + hdr + ">");
                    any_fix = true;
                }
            }
        }
    }

    // ── [2] Undeclared identifiers / missing symbols ─────────────────────────
    {
        std::regex re(R"((?:use of undeclared identifier|undeclared identifier|implicit declaration of function) '([^']+)')");
        std::set<std::string> syms;
        for (std::sregex_iterator it(error_log.begin(), error_log.end(), re);
             it != std::sregex_iterator{}; ++it)
            syms.insert((*it)[1].str());

        static const std::map<std::string, std::string> sym_fixes = {
            {"MSG_NOSIGNAL",
             "#ifndef MSG_NOSIGNAL\n#define MSG_NOSIGNAL 0\n#endif"},
            {"O_TMPFILE",
             "#ifndef O_TMPFILE\n#define O_TMPFILE 0\n#endif"},
            {"CLOCK_MONOTONIC_RAW",
             "#ifndef CLOCK_MONOTONIC_RAW\n#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC\n#endif"},
            {"CLOCK_BOOTTIME",
             "#ifndef CLOCK_BOOTTIME\n#define CLOCK_BOOTTIME CLOCK_MONOTONIC\n#endif"},
            {"CLOCK_REALTIME_COARSE",
             "#ifndef CLOCK_REALTIME_COARSE\n#define CLOCK_REALTIME_COARSE CLOCK_REALTIME\n#endif"},
            {"CLOCK_MONOTONIC_COARSE",
             "#ifndef CLOCK_MONOTONIC_COARSE\n#define CLOCK_MONOTONIC_COARSE CLOCK_MONOTONIC\n#endif"},
            {"pipe2",
             "#ifdef __APPLE__\n"
             "static inline int pipe2(int fd[2], int flags) { (void)flags; return pipe(fd); }\n"
             "#endif"},
            {"accept4",
             "#ifdef __APPLE__\n"
             "#include <sys/socket.h>\n"
             "static inline int accept4(int s, struct sockaddr* a,\n"
             "                          socklen_t* l, int f) {\n"
             "    (void)f; return accept(s, a, l);\n"
             "}\n"
             "#endif"},
            {"getauxval",
             "#ifndef getauxval\n"
             "static inline unsigned long getauxval(unsigned long t) { (void)t; return 0; }\n"
             "#endif"},
            {"TEMP_FAILURE_RETRY",
             "#ifndef TEMP_FAILURE_RETRY\n"
             "#include <errno.h>\n"
             "#define TEMP_FAILURE_RETRY(e) \\\n"
             "    ({ __typeof__(e) _r; do { _r=(e); } while (_r==-1 && errno==EINTR); _r; })\n"
             "#endif"},
            {"malloc_usable_size",
             "#ifdef __APPLE__\n"
             "#include <malloc/malloc.h>\n"
             "#define malloc_usable_size(p) malloc_size(p)\n"
             "#endif"},
            {"__pid_t",
             "#ifdef __APPLE__\n"
             "#ifndef __pid_t\ntypedef pid_t __pid_t;\n#endif\n"
             "#endif"},
            {"memrchr",
             "#ifdef __APPLE__\n"
             "#include <stddef.h>\n"
             "static inline void* memrchr(const void* s, int c, size_t n) {\n"
             "    const unsigned char* p = (const unsigned char*)s + n;\n"
             "    while (n--) if (*--p == (unsigned char)c) return (void*)p;\n"
             "    return 0;\n"
             "}\n"
             "#endif"},
            {"strchrnul",
             "#ifdef __APPLE__\n"
             "static inline char* strchrnul(const char* s, int c) {\n"
             "    while (*s && *s != (char)c) ++s; return (char*)s;\n"
             "}\n"
             "#endif"},
            {"strdupa",
             "#ifdef __APPLE__\n"
             "#include <alloca.h>\n"
             "#include <string.h>\n"
             "#define strdupa(s) \\\n"
             "    ({ size_t _n=strlen(s)+1; char* _d=(char*)alloca(_n); memcpy(_d,s,_n); _d; })\n"
             "#endif"},
            {"fallocate",
             "#ifdef __APPLE__\n"
             "#include <unistd.h>\n"
             "static inline int fallocate(int fd, int m, off_t o, off_t l) {\n"
             "    (void)m; return ftruncate(fd, o + l);\n"
             "}\n"
             "#endif"},
            {"getrandom",
             "#ifdef __APPLE__\n"
             "#include <sys/types.h>\n"
             "extern int getentropy(void* buf, size_t len);\n"
             "static inline ssize_t getrandom(void* buf, size_t n, unsigned f) {\n"
             "    (void)f; return getentropy(buf, n) == 0 ? (ssize_t)n : -1;\n"
             "}\n"
             "#endif"},
            {"explicit_bzero",
             "#ifdef __APPLE__\n"
             "#include <strings.h>\n"
             "#ifndef explicit_bzero\n"
             "static inline void explicit_bzero(void* p, size_t n) { memset(p, 0, n); }\n"
             "#endif\n"
             "#endif"},
            {"reallocarray",
             "#ifdef __APPLE__\n"
             "#include <stdlib.h>\n"
             "#ifndef reallocarray\n"
             "static inline void* reallocarray(void* p, size_t nm, size_t sz) {\n"
             "    return realloc(p, nm * sz);\n"
             "}\n"
             "#endif\n"
             "#endif"},
            {"strlcpy",
             "/* strlcpy is already in macOS libc — no action needed */"},
            {"strlcat",
             "/* strlcat is already in macOS libc — no action needed */"},
        };

        for (const auto& sym : syms) {
            auto it = sym_fixes.find(sym);
            if (it != sym_fixes.end()) {
                colors::print_substatus("Self-healing: patching undefined symbol '" + sym + "'");
                if (append_if_new(it->second)) any_fix = true;
            }
        }
    }

    // ── [3] Linker: library not found or undefined symbols ──────────────────
    {
        // macOS ld: "library not found for -lFOO"
        std::regex re_lib("library not found for -l(\\S+)");
        // undefined symbol: "_libintl_setlocale" or "_libintl_setlocale", referenced from:
        std::regex re_sym("undefined symbol: \"?(_\\S+)\"?");
        std::regex re_sym2("\"?(_\\S+)\"?, referenced from:");
        std::regex re_sym3("symbol\\(s\\) not found.*$"); // Trigger for general symbol check

        static const std::set<std::string> noop_libs = {
            "rt", "dl", "pthread", "resolv", "atomic", "m" /* libm is in libSystem */
        };

        // Symbol to library mapping
        static const std::map<std::string, std::string> sym_to_lib = {
            {"libintl_setlocale", "intl"},
            {"libintl_gettext", "intl"},
            {"libintl_textdomain", "intl"},
            {"iconv_open", "iconv"},
            {"iconv_close", "iconv"},
            {"iconv", "iconv"},
            {"inflate", "z"},
            {"deflate", "z"}
        };

        std::set<std::string> seen_libs;
        for (std::sregex_iterator it(error_log.begin(), error_log.end(), re_lib);
             it != std::sregex_iterator{}; ++it) {
            std::string lib = (*it)[1].str();
            if (!seen_libs.insert(lib).second) continue;
            if (noop_libs.count(lib)) {
// Silent fix
                patch_build_flags(src_dir, "-l" + lib, "");
                any_fix = true;
            }
        }

        std::set<std::string> missing_libs;
        auto check_sym = [&](std::string sym) {
            if (sym.empty()) return;
            if (sym[0] == '_') sym = sym.substr(1);
            auto it = sym_to_lib.find(sym);
            if (it != sym_to_lib.end()) {
                missing_libs.insert(it->second);
            }
        };

        for (std::sregex_iterator it(error_log.begin(), error_log.end(), re_sym); it != std::sregex_iterator{}; ++it)
            check_sym((*it)[1].str());
        for (std::sregex_iterator it(error_log.begin(), error_log.end(), re_sym2); it != std::sregex_iterator{}; ++it)
            check_sym((*it)[1].str());

        // Fallback: if we see general symbol error, scan the whole log for known problematic symbols
        if (std::regex_search(error_log, re_sym3)) {
            for (const auto& [sym, lib] : sym_to_lib) {
                if (log.find(sym) != std::string::npos) {
                    missing_libs.insert(lib);
                }
            }
        }

        for (const auto& lib : missing_libs) {
            std::string flag = " -l" + lib;
            if (extra_cflags.find(flag) == std::string::npos) {
                colors::print_substatus("Self-healing: injecting missing library flag " + flag);
                extra_cflags += flag;
                any_fix = true;
            }
        }
    }

    // ── [4] GNU ld options not supported by Apple ld ─────────────────────────
    {
        static const std::vector<std::string> gnu_ld_opts = {
            "-Wl,--as-needed",       "-Wl,--no-as-needed",
            "-Wl,--no-undefined",    "-Wl,--allow-shlib-undefined",
            "-Wl,-z,relro",          "-Wl,-z,now",
            "-Wl,--gc-sections",     "-Wl,-export-dynamic",
            "-Wl,--version-script",  "-Wl,-soname",
        };
        for (const auto& opt : gnu_ld_opts) {
            // Apple ld prints "unknown option: --X" — check inner option name
            std::string inner = opt;
            if (opt.find("-Wl,") == 0) inner = opt.substr(4);
            
            if ((log.find("unknown option") != std::string::npos &&
                log.find(inner) != std::string::npos) || 
                (log.find("unrecognized option") != std::string::npos && 
                log.find(inner) != std::string::npos)) {
                colors::print_substatus("Self-healing: removing GNU ld flag " + opt);
                patch_build_flags(src_dir, opt, "");
                any_fix = true;
            }
        }
    }

    // ── [5] Unknown compiler flags / Missing tools ───────────────────────────
    {
        // clang: "unknown warning option '-Wfoo'"
        // clang: "error: unknown argument: '-ffoo'"
        // clang: "unsupported option '-ffoo'"
        std::regex re_warn  (R"(unknown warning option '(-[^']+)')");
        std::regex re_arg   (R"(error: unknown argument: '(-[^']+)')");
        std::regex re_unsup (R"(unsupported option '(-[^']+)')");
        std::regex re_notfound(R"(([^\s/]+): command not found)");

        std::set<std::string> bad_flags;
        for (auto& re : {re_warn, re_arg, re_unsup})
            for (std::sregex_iterator it(error_log.begin(), error_log.end(), re);
                 it != std::sregex_iterator{}; ++it)
                bad_flags.insert((*it)[1].str());

        for (const auto& flag : bad_flags) {
            colors::print_substatus("Self-healing: removing unsupported flag " + flag);
            patch_build_flags(src_dir, flag, "");
            any_fix = true;
        }

        static const std::map<std::string, std::string> tool_to_brew = {
            {"aclocal", "automake"},
            {"automake", "automake"},
            {"autoconf", "autoconf"},
            {"autoheader", "autoconf"},
            {"libtool", "libtool"},
            {"glibtool", "libtool"},
            {"pkg-config", "pkg-config"},
            {"m4", "m4"},
            {"bison", "bison"},
            {"flex", "flex"},
            {"gettext", "gettext"},
            {"msgfmt", "gettext"}
        };

        for (std::sregex_iterator it(error_log.begin(), error_log.end(), re_notfound); it != std::sregex_iterator{}; ++it) {
            std::string tool = (*it)[1].str();
            auto it2 = tool_to_brew.find(tool);
            if (it2 != tool_to_brew.end()) {
                std::string brew_prefix = fs::exists("/opt/homebrew/bin") ? "/opt/homebrew" : "/usr/local";
                if (!fs::exists(brew_prefix + "/bin/" + it2->second)) {
                    colors::print_substatus("Self-healing: installing missing tool " + it2->second);
                    setenv("HOMEBREW_NO_INTERACTIVE", "1", 1);
                    run_exec(brew_prefix + "/bin/brew", {"install", "--formula", it2->second});
                    any_fix = true;
                }
            }
        }
    }

    // ── [6] VFS Mocking / Linux path redirection ─────────────────────────────
    {
        std::regex re_path("\"(/(?:proc|sys|etc)/\\S+)\"");
        std::set<std::string> paths;
        for (std::sregex_iterator it = std::sregex_iterator(error_log.begin(), error_log.end(), re_path); it != std::sregex_iterator{}; ++it)
            paths.insert((*it)[1].str());

        for (const auto& p : paths) {
            if (p.find("/proc/cpuinfo") != std::string::npos) {
                std::string fake_cpu = "/tmp/macman_cpuinfo";
                if (!fs::exists(fake_cpu)) {
                    std::ofstream f(fake_cpu);
                    f << "processor\t: 0\nvendor_id\t: Apple\ncpu family\t: 6\nmodel\t\t: 0\nmodel name\t: Apple M-series\n";
                }
                append_if_new("#define MACMAN_FAKE_PROC_CPUINFO \"" + fake_cpu + "\"\n");
                patch_build_flags(src_dir, p, "MACMAN_FAKE_PROC_CPUINFO");
                any_fix = true;
            }
        }
    }

    // ── [6] CMake "Could NOT find <Package>" ─────────────────────────────────
    {
        std::regex re(R"(Could NOT find (\w[\w\-\.]+))");
        std::set<std::string> missing_pkgs;
        for (std::sregex_iterator it(error_log.begin(), error_log.end(), re);
             it != std::sregex_iterator{}; ++it)
            missing_pkgs.insert((*it)[1].str());

        for (const auto& pkg : missing_pkgs) {
            colors::print_substatus("Self-healing: relaxing CMake REQUIRED for " + pkg);
            patch_cmake_remove_required(src_dir, pkg);
            any_fix = true;
        }
    }

    // ── [7] CMake: missing executable (sphinx, doxygen, …) ───────────────────
    {
        std::regex re(R"((?:sphinx-build|doxygen)[^'"\n]*(?:not found|NOTFOUND))");
        if (std::regex_search(error_log, re)) {
            patch_cmake_remove_required(src_dir, "Sphinx");
            patch_cmake_remove_required(src_dir, "Doxygen");
            any_fix = true;
        }
    }

    // ── [8] Write compat header & inject -include into extra_cflags ──────────
    if (any_fix) {
        {
            std::ofstream f(compat_h);
            f << compat_content;
        }
        std::string inject = " -I'" + compat_dir + "' -include '" + compat_h + "'";
        if (extra_cflags.find(compat_dir) == std::string::npos)
            extra_cflags += inject;
    }

    return any_fix;
}

bool Installer::build_with_healing(const std::string& base_cmd,
                                    const std::string& src_dir,
                                    int max_retries) const {
    std::string compat_dir = src_dir + "/.macman_compat";
    fs::create_directories(compat_dir);

    std::string brew_prefix =
        fs::exists("/opt/homebrew/bin") ? "/opt/homebrew" : "/usr/local";

    std::string extra_cflags;   // grows with each fix round
    std::string build_output;
    int ret = -1;

    for (int attempt = 0; attempt < max_retries; ++attempt) {
        if (attempt == 0) {
            colors::print_substatus("Compiling " + src_dir.substr(src_dir.rfind('/') + 1) + "...");
        } else {
            colors::print_substatus("Healing... (Retry " + std::to_string(attempt) + ")");
        }

        // Re-compose the full command on every attempt so accumulated
        // extra_cflags (−I/−include) are picked up by the compiler.
        std::string cmd =
            "export CC=clang CXX=clang++ && "
            "export CFLAGS='-O2 -I" + brew_prefix + "/include" + extra_cflags + "' && "
            "export CXXFLAGS='-O2 -std=c++17 -I" + brew_prefix + "/include" + extra_cflags + "' && "
            "export LDFLAGS='-L" + brew_prefix + "/lib' && "
            "export PKG_CONFIG_PATH='" + brew_prefix + "/lib/pkgconfig:"
                                       + brew_prefix + "/share/pkgconfig' && "
            + base_cmd;

        ret = run_capturing_output(cmd, build_output);
        if (ret == 0) break;

        if (attempt < max_retries - 1) {
            colors::print_warning("Build failed (exit " + std::to_string(ret) +
                                  "). Analyzing compiler output...");
            bool fixed = analyze_and_fix_compile_errors(
                build_output, src_dir, compat_dir, extra_cflags);

            if (!fixed) {
                colors::print_error("No automatic fix could be applied.");
                break;
            }
        }
    }

    if (ret != 0) {
        colors::print_error("Build failed after " + std::to_string(max_retries) +
                            " attempt(s). Last compiler output:");
        std::istringstream ss(build_output);
        std::vector<std::string> lines;
        std::string ln;
        while (std::getline(ss, ln)) lines.push_back(ln);
        size_t start = lines.size() > 25 ? lines.size() - 25 : 0;
        for (size_t i = start; i < lines.size(); ++i)
            std::cerr << "  " << lines[i] << '\n';
        return false;
    }

    if (fs::exists(compat_dir))
        fs::remove_all(compat_dir);   // Clean up compat stubs on success

    return true;
}

} // namespace macman
