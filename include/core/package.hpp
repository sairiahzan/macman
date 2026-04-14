// package.hpp — Package Data Structure [V1.1.0 Patch]
// Defines the Package struct that represents a software package throughout
// macman. Includes JSON serialization/deserialization and version comparison.


#pragma once

#include <string>
#include <vector>
#include <ctime>
#include <nlohmann/json.hpp>

namespace macman {

// --- Package Source Enum ---

enum class PackageSource {
    HOMEBREW,   // Installed from Homebrew formulae API
    AUR,        // Built from Arch User Repository source
    LOCAL       // Manually installed / unknown source
};

// Convert PackageSource to/from string for serialization
inline std::string source_to_string(PackageSource src) {
    switch (src) {
        case PackageSource::HOMEBREW: return "homebrew";
        case PackageSource::AUR:     return "aur";
        case PackageSource::LOCAL:   return "local";
    }
    return "unknown";
}

inline PackageSource string_to_source(const std::string& s) {
    if (s == "homebrew") return PackageSource::HOMEBREW;
    if (s == "aur")      return PackageSource::AUR;
    return PackageSource::LOCAL;
}

// --- Package Struct ---

struct Package {
    std::string name;                       // Package name (e.g., "wget")
    std::string version;                    // Version string (e.g., "1.21.4")
    std::string description;                // Short description
    std::string homepage;                   // Homepage URL
    std::string url;                        // Download URL (bottle or source)
    std::string sha256;                     // SHA256 checksum of the download
    size_t      installed_size = 0;         // Installed size in bytes
    size_t      download_size  = 0;         // Download size in bytes
    PackageSource source = PackageSource::HOMEBREW;

    std::vector<std::string> dependencies;  // Runtime dependencies
    std::vector<std::string> build_deps;    // Build-time dependencies
    std::vector<std::string> installed_files; // Files owned by this package
    
    std::string install_date;               // ISO 8601 install timestamp
    std::string install_reason;             // "explicit" or "dependency"

    // --- Version Comparison ---

    bool operator==(const Package& other) const {
        return name == other.name && version == other.version;
    }

    bool operator!=(const Package& other) const {
        return !(*this == other);
    }

    static int compare_versions(const std::string& v1, const std::string& v2);

    // --- JSON Serialization ---

    nlohmann::json to_json() const;
    static Package from_json(const nlohmann::json& j);

    // --- Display Helpers ---

    std::string format_size(size_t bytes) const;
    std::string summary_line() const;
};

} // namespace macman
