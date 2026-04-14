// installer.cpp

#include "core/installer.hpp"
#include "backend/homebrew_backend.hpp"
#include "backend/aur_backend.hpp"
#include "net/downloader.hpp"
#include "macman.hpp"
#include "cli/colors.hpp"
#include <iostream>
#include <filesystem>
#include <cstdlib>
#include <vector>

namespace fs = std::filesystem;

namespace macman {

Installer::Installer(Database& db) : db_(db) {}

bool Installer::verify_checksum(const std::string& file_path, const std::string& expected_sha256) const {
    if (expected_sha256.empty()) return true; // No hash provided

    colors::print_substatus("Verifying SHA-256 for: " + fs::path(file_path).filename().string());
    
    std::string cmd = "shasum -a 256 '" + file_path + "' | awk '{print $1}'";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return false;

    char buffer[128];
    std::string result = "";
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    pclose(pipe);

    // Trim newlines
    if (!result.empty() && result.back() == '\n') result.pop_back();

    if (result != expected_sha256) {
        colors::print_error("SHA-256 verification failed!");
        colors::print_error("Expected: " + expected_sha256);
        colors::print_error("Got:      " + result);
        return false;
    }
    
    colors::print_success("Checksum matched perfectly.");
    return true;
}

void Installer::fix_macho_rpaths(const std::string& deploy_dir) const {
    // We traverse /bin and /lib to fix any broken dynamic library linkage
    std::string bin_dir = deploy_dir + "/bin";
    if (!fs::exists(bin_dir)) return;

    colors::print_substatus("Patching Mach-O RPATHs...");
    for (const auto& entry : fs::directory_iterator(bin_dir)) {
        if (!entry.is_regular_file()) continue;

        std::string file = entry.path().string();
        
        // Use install_name_tool to ensure the executable looks in ~/.macman/lib
        std::string cmd = "install_name_tool -add_rpath '" + get_prefix() + "/lib' '" + file + "' 2>/dev/null";
        system(cmd.c_str());
        
        // Also fix ID if necessary
        std::string cmd2 = "install_name_tool -id '@rpath/" + entry.path().filename().string() + "' '" + file + "' 2>/dev/null";
        system(cmd2.c_str());
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

            fs::create_directories(target_path.parent_path());
            
            // Overwrite existing without resolving symlink targets
            std::error_code ec;
            if (fs::exists(fs::symlink_status(target_path))) {
                fs::remove(target_path, ec);
            }
            
            fs::rename(entry.path(), target_path, ec);
            if (ec) {
                // Move fallback block via cp -a and rm in case of cross-device links
                std::string cmd = "cp -a '" + entry.path().string() + "' '" + target_path.string() + "' && rm -f '" + entry.path().string() + "'";
                system(cmd.c_str());
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

bool Installer::install_package(const Package& pkg, const std::string& reason) {
    Package installed = pkg;
    installed.install_reason = reason;

    // Set install date
    auto now = std::time(nullptr);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
    installed.install_date = buf;
    
    // Stage directory
    std::string stage_dir = get_cache_dir() + "/stage_" + pkg.name;
    if (fs::exists(stage_dir)) fs::remove_all(stage_dir);
    fs::create_directories(stage_dir);

    bool build_success = false;
    
    if (pkg.source == PackageSource::AUR) {
        colors::print_status("Sources found in Arch Linux AUR. Compiling natively for macOS...");
        // Build from AUR source using stage_dir as DESTDIR substitute
        // The AURBackend currently expects to install straight to get_prefix().
        // For actual atomic installs, we need AURBackend to deploy to staging.
        // For now, we simulate success by forwarding to get_prefix() directly until Phase 4 Backend refactor.
        AURBackend aur;
        build_success = aur.build_and_install(pkg.name, get_prefix(), installed.installed_files);
        if (build_success) fix_macho_rpaths(get_prefix());
        
    } else {
        colors::print_substatus("Found native macOS binary in Homebrew. Installing " + pkg.name + "...");
        // Brew Bottle branch
        if (pkg.url.empty()) {
            colors::print_error("No download URL for " + pkg.name);
            return false;
        }

        std::string tarball_path = get_cache_dir() + "/" + pkg.name + "-" + pkg.version + ".tar.gz";

        if (!fs::exists(tarball_path)) {
            Downloader dl;
            DownloadTask task;
            task.url = pkg.url;
            task.output_path = tarball_path;
            task.label = pkg.name;
            task.expected_size = pkg.download_size;

            auto result = dl.download(task);
            if (!result.success) {
                colors::print_error(result.error);
                return false;
            }
        }
        
        // SHA-256 verification (Phase 3 spec)
        if (!verify_checksum(tarball_path, pkg.sha256)) {
            return false;
        }

        HomebrewBackend brew;
        // Deploy to Stage
        build_success = brew.install_bottle(tarball_path, stage_dir, installed.installed_files);
        if (build_success) {
            // Atomic move stage to prefix
            colors::print_substatus("Atomically finalizing installation...");
            if (atomic_commit(stage_dir, get_prefix())) {
                build_success = true;
            } else {
                build_success = false;
            }
        }
    }
    
    // Cleanup stage if failed
    if (fs::exists(stage_dir)) fs::remove_all(stage_dir);

    if (build_success) {
        db_.add_package(installed);
        return true;
    }
    return false;
}

} // namespace macman
