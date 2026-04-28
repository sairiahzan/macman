// Arda Yiğit - Hazani
// argument_parser.hpp — CLI Argument Parser
// Parses pacman-compatible command-line arguments and maps them to
// operations. Handles all -S, -R, -Q flags and their sub-options.


#pragma once

#include <string>
#include <vector>

namespace macman {

// --- Operation Types ---

enum class Operation {
    NONE,
    SYNC_INSTALL,       // -S <pkg>     : Install package(s)
    SYNC_SEARCH,        // -Ss <query>  : Search packages
    SYNC_INFO,          // -Si <pkg>    : Show package info
    SYNC_REFRESH,       // -Sy          : Refresh package database
    SYNC_UPGRADE,       // -Syu         : Full system upgrade
    REMOVE,             // -R <pkg>     : Remove package
    REMOVE_RECURSIVE,   // -Rs <pkg>    : Remove with orphan deps
    QUERY_LIST,         // -Q           : List installed packages
    QUERY_INFO,         // -Qi <pkg>    : Info on installed package
    QUERY_FILES,        // -Ql <pkg>    : List files of package
    QUERY_OWNS,         // -Qo <file>   : Find which package owns file
    NUKE_ALL,           // --nuke       : Remove all packages completely
    VERSION,            // --version    : Show version
    HELP                // -h / --help  : Show help
};

// --- Parsed Arguments ---

struct ParsedArgs {
    Operation operation = Operation::NONE;
    std::vector<std::string> targets;       // Package names or search queries
    bool no_confirm = false;                // --noconfirm
    bool verbose = false;                   // -v / --verbose
    bool color = true;                      // --color=auto/always/never
};

// --- Argument Parser Class ---

class ArgumentParser {
public:
    ArgumentParser() = default;
    ~ArgumentParser() = default;

    // --- Parse Arguments ---

    ParsedArgs parse(int argc, char* argv[]);

    // --- Help & Usage ---

    static void print_help();
    static void print_version();
    static void print_usage();

private:
    // --- Sub-parsers ---

    Operation parse_sync_flags(const std::string& flags) const;
    Operation parse_remove_flags(const std::string& flags) const;
    Operation parse_query_flags(const std::string& flags) const;
};

} // namespace macman
