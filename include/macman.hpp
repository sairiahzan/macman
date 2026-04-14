// macman.hpp — Global Constants, Version Info, and System Paths
// Central header that defines all compile-time constants used across macman.
// Includes version string, directory paths, API endpoints, and branding.


#pragma once

#include <string>

namespace macman {

// --- Version & Branding ---

constexpr const char* VERSION       = "1.1.0";
constexpr const char* PROGRAM_NAME  = "macman";
constexpr const char* DESCRIPTION   = "The blazing-fast package manager for macOS";

// --- System Paths ---
// All paths under $HOME/.macman — no sudo required (Homebrew-style user-space)

#include <cstdlib>

inline std::string get_macman_root() {
    // When running under sudo, HOME points to /var/root which has no macman data.
    // Use SUDO_USER to find the real user's home directory.
    const char* sudo_user = std::getenv("SUDO_USER");
    if (sudo_user) {
        return std::string("/Users/") + sudo_user + "/.macman";
    }
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "/tmp") + "/.macman";
}

inline std::string get_prefix()       { return get_macman_root(); }
inline std::string get_bin_dir()      { return get_macman_root() + "/bin"; }
inline std::string get_config_file()  { return get_macman_root() + "/etc/macman.conf"; }
inline std::string get_db_dir()       { return get_macman_root() + "/var"; }
inline std::string get_local_db()     { return get_macman_root() + "/var/local.db"; }
inline std::string get_sync_db_dir()  { return get_macman_root() + "/var/sync"; }
inline std::string get_cache_dir()    { return get_macman_root() + "/var/cache"; }
inline std::string get_log_file()     { return get_macman_root() + "/var/macman.log"; }

// Legacy compile-time constants (kept for backward compat, point to new paths)
constexpr const char* PREFIX            = "/usr/local";  // Only used by AUR DESTDIR staging

// --- Homebrew API Endpoints ---

constexpr const char* BREW_API_BASE     = "https://formulae.brew.sh/api";
constexpr const char* BREW_FORMULA_LIST = "https://formulae.brew.sh/api/formula.json";
constexpr const char* BREW_CASK_LIST    = "https://formulae.brew.sh/api/cask.json";

// Constructed at runtime: BREW_API_BASE + "/formula/<name>.json"
// Constructed at runtime: BREW_API_BASE + "/cask/<name>.json"

// --- AUR API Endpoints ---

constexpr const char* AUR_RPC_BASE      = "https://aur.archlinux.org/rpc/";
constexpr const char* AUR_PACKAGE_BASE  = "https://aur.archlinux.org/cgit/aur.git/snapshot/";

// --- Network Settings ---

constexpr int    HTTP_TIMEOUT_SECONDS   = 30;
constexpr int    DOWNLOAD_TIMEOUT_SECS  = 300;
constexpr int    MAX_RETRIES            = 3;
constexpr size_t MAX_PARALLEL_DOWNLOADS = 4;

// --- ASCII Art Banner ---

constexpr const char* BANNER = R"(
                                            
  ███╗   ███╗ █████╗  ██████╗███╗   ███╗ █████╗ ███╗   ██╗
  ████╗ ████║██╔══██╗██╔════╝████╗ ████║██╔══██╗████╗  ██║
  ██╔████╔██║███████║██║     ██╔████╔██║███████║██╔██╗ ██║
  ██║╚██╔╝██║██╔══██║██║     ██║╚██╔╝██║██╔══██║██║╚██╗██║
  ██║ ╚═╝ ██║██║  ██║╚██████╗██║ ╚═╝ ██║██║  ██║██║ ╚████║
  ╚═╝     ╚═╝╚═╝  ╚═╝ ╚═════╝╚═╝     ╚═╝╚═╝  ╚═╝╚═╝  ╚═══╝
                                            
)";

} // namespace macman
