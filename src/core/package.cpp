// package.cpp — Package Struct Implementation
// Implements JSON serialization/deserialization, version comparison,
// and human-readable formatting for the Package data structure.


#include "core/package.hpp"
#include "cli/colors.hpp"
#include <sstream>
#include <iomanip>
#include <cmath>

namespace macman {

// --- JSON Serialization ---

nlohmann::json Package::to_json() const {
    return nlohmann::json{
        {"name",            name},
        {"version",         version},
        {"description",     description},
        {"homepage",        homepage},
        {"url",             url},
        {"sha256",          sha256},
        {"installed_size",  installed_size},
        {"download_size",   download_size},
        {"source",          source_to_string(source)},
        {"dependencies",    dependencies},
        {"build_deps",      build_deps},
        {"installed_files", installed_files},
        {"install_date",    install_date},
        {"install_reason",  install_reason}
    };
}

// --- JSON Deserialization ---

Package Package::from_json(const nlohmann::json& j) {
    Package pkg;
    
    // Safely extract fields with defaults
    pkg.name            = j.value("name", "");
    pkg.version         = j.value("version", "");
    pkg.description     = j.value("description", "");
    pkg.homepage        = j.value("homepage", "");
    pkg.url             = j.value("url", "");
    pkg.sha256          = j.value("sha256", "");
    pkg.installed_size  = j.value("installed_size", (size_t)0);
    pkg.download_size   = j.value("download_size", (size_t)0);
    pkg.source          = string_to_source(j.value("source", "local"));
    pkg.install_date    = j.value("install_date", "");
    pkg.install_reason  = j.value("install_reason", "explicit");

    if (j.contains("dependencies") && j["dependencies"].is_array()) {
        pkg.dependencies = j["dependencies"].get<std::vector<std::string>>();
    }
    if (j.contains("build_deps") && j["build_deps"].is_array()) {
        pkg.build_deps = j["build_deps"].get<std::vector<std::string>>();
    }
    if (j.contains("installed_files") && j["installed_files"].is_array()) {
        pkg.installed_files = j["installed_files"].get<std::vector<std::string>>();
    }

    return pkg;
}

// --- Human-Readable Size Formatting ---

std::string Package::format_size(size_t bytes) const {
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int unit_index = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024.0 && unit_index < 4) {
        size /= 1024.0;
        unit_index++;
    }

    std::ostringstream oss;
    if (unit_index == 0) {
        oss << bytes << " " << units[unit_index];
    } else {
        oss << std::fixed << std::setprecision(1) << size << " " << units[unit_index];
    }
    return oss.str();
}

// --- Summary Line (for search results) ---

std::string Package::summary_line() const {
    std::ostringstream oss;
    
    // Format: "repo/name version (source)"
    //         "    description"
    std::string src_label;
    switch (source) {
        case PackageSource::HOMEBREW: src_label = "homebrew"; break;
        case PackageSource::AUR:     src_label = "aur"; break;
        case PackageSource::LOCAL:   src_label = "local"; break;
    }

    oss << colors::BOLD_MAGENTA << src_label << "/" 
        << colors::BOLD_WHITE << name << " "
        << colors::BOLD_GREEN << version << colors::RESET;
    
    if (installed_size > 0) {
        oss << " " << colors::BOLD_CYAN << "(" << format_size(installed_size) << ")" 
            << colors::RESET;
    }
    
    oss << "\n    " << description;
    
    return oss.str();
}

} // namespace macman
