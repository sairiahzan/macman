// Arda Yiğit - Hazani
// argument_parser.cpp — CLI Argument Parser Implementation
// Parses command-line arguments in pacman style (-S, -Ss, -Syu, etc.)
// and generates help/usage/version output.


#include "cli/argument_parser.hpp"
#include "cli/colors.hpp"
#include "macman.hpp"
#include <iostream>
#include <algorithm>

namespace macman {

// --- Parse Sync Flags (-S...) ---

Operation ArgumentParser::parse_sync_flags(const std::string& flags) const {
    // -Syu (has both y and u)
    if (flags.find('y') != std::string::npos && flags.find('u') != std::string::npos) {
        return Operation::SYNC_UPGRADE;
    }
    // -Sy (refresh only)
    if (flags.find('y') != std::string::npos) {
        return Operation::SYNC_REFRESH;
    }
    // -Ss (search)
    if (flags.find('s') != std::string::npos) {
        return Operation::SYNC_SEARCH;
    }
    // -Si (info)
    if (flags.find('i') != std::string::npos) {
        return Operation::SYNC_INFO;
    }
    // -S (install)
    return Operation::SYNC_INSTALL;
}

// --- Parse Remove Flags (-R...) ---

Operation ArgumentParser::parse_remove_flags(const std::string& flags) const {
    // -Rs (recursive removal)
    if (flags.find('s') != std::string::npos) {
        return Operation::REMOVE_RECURSIVE;
    }
    // -R (simple remove)
    return Operation::REMOVE;
}

// --- Parse Query Flags (-Q...) ---

Operation ArgumentParser::parse_query_flags(const std::string& flags) const {
    // -Qi (info)
    if (flags.find('i') != std::string::npos) {
        return Operation::QUERY_INFO;
    }
    // -Ql (file list)
    if (flags.find('l') != std::string::npos) {
        return Operation::QUERY_FILES;
    }
    // -Qo (file ownership)
    if (flags.find('o') != std::string::npos) {
        return Operation::QUERY_OWNS;
    }
    // -Q (list all)
    return Operation::QUERY_LIST;
}

// --- Main Parse Function ---

ParsedArgs ArgumentParser::parse(int argc, char* argv[]) {
    ParsedArgs args;

    if (argc < 2) {
        args.operation = Operation::HELP;
        return args;
    }

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        // Long options
        if (arg == "--help") {
            args.operation = Operation::HELP;
            return args;
        }
        if (arg == "--version") {
            args.operation = Operation::VERSION;
            return args;
        }
        if (arg == "--nuke") {
            args.operation = Operation::NUKE_ALL;
            continue;
        }
        if (arg == "--noconfirm") {
            args.no_confirm = true;
            continue;
        }
        if (arg == "--verbose" || arg == "-v") {
            args.verbose = true;
            continue;
        }
        if (arg.find("--color=") == 0) {
            std::string val = arg.substr(8);
            args.color = (val != "never");
            continue;
        }

        // Short options (pacman-style)
        if (arg.length() >= 2 && arg[0] == '-' && arg[1] != '-') {
            char op = arg[1]; // Primary operation character
            std::string sub_flags = arg.substr(2); // Additional flags

            switch (op) {
                case 'S':
                    args.operation = parse_sync_flags(sub_flags);
                    break;
                case 'R':
                    args.operation = parse_remove_flags(sub_flags);
                    break;
                case 'Q':
                    args.operation = parse_query_flags(sub_flags);
                    break;
                case 'h':
                    args.operation = Operation::HELP;
                    return args;
                default:
                    colors::print_error("invalid option -- '" + std::string(1, op) + "'");
                    args.operation = Operation::HELP;
                    return args;
            }
            continue;
        }

        // Everything else is a target (package name or search query)
        args.targets.push_back(arg);
    }

    return args;
}

// --- Print Version ---

void ArgumentParser::print_version() {
    std::cout << colors::BOLD_CYAN;
    std::cout << BANNER;
    std::cout << colors::RESET;
    std::cout << "  " << colors::BOLD_WHITE << PROGRAM_NAME << colors::RESET 
              << " v" << VERSION << std::endl;
    std::cout << "  " << DESCRIPTION << std::endl;
    std::cout << "  " << colors::DIM << "(c) 2026 Arda Yigit - Hazani" << colors::RESET << std::endl;
    std::cout << std::endl;
}

// --- Print Usage ---

void ArgumentParser::print_usage() {
    std::cout << "usage:  " << colors::BOLD_WHITE << "macman" << colors::RESET 
              << " <operation> [options] [targets]" << std::endl;
    std::cout << "        " << colors::BOLD_WHITE << "macman" << colors::RESET 
              << " {-h --help}" << std::endl;
}

// --- Print Full Help ---

void ArgumentParser::print_help() {
    print_version();
    print_usage();
    
    std::cout << std::endl;
    std::cout << colors::BOLD_WHITE << "Operations:" << colors::RESET << std::endl;
    
    std::cout << "  " << colors::BOLD_GREEN << "-S" << colors::RESET 
              << "  <pkg>     Install a package" << std::endl;
    std::cout << "  " << colors::BOLD_GREEN << "-Ss" << colors::RESET 
              << " <query>   Search for a package" << std::endl;
    std::cout << "  " << colors::BOLD_GREEN << "-Si" << colors::RESET 
              << " <pkg>     Display information about a package" << std::endl;
    std::cout << "  " << colors::BOLD_GREEN << "-Sy" << colors::RESET 
              << "           Refresh package databases" << std::endl;
    std::cout << "  " << colors::BOLD_GREEN << "-Syu" << colors::RESET 
              << "          Full system upgrade" << std::endl;

    std::cout << std::endl;
    
    std::cout << "  " << colors::BOLD_RED << "-R" << colors::RESET 
              << "  <pkg>     Remove a package" << std::endl;
    std::cout << "  " << colors::BOLD_RED << "-Rs" << colors::RESET 
              << " <pkg>     Remove a package and its orphan dependencies" << std::endl;

    std::cout << std::endl;

    std::cout << "  " << colors::BOLD_CYAN << "-Q" << colors::RESET 
              << "            List all installed packages" << std::endl;
    std::cout << "  " << colors::BOLD_CYAN << "-Qi" << colors::RESET 
              << " <pkg>     Show info about an installed package" << std::endl;
    std::cout << "  " << colors::BOLD_CYAN << "-Ql" << colors::RESET 
              << " <pkg>     List files owned by a package" << std::endl;
    std::cout << "  " << colors::BOLD_CYAN << "-Qo" << colors::RESET 
              << " <file>    Find which package owns a file" << std::endl;

    std::cout << std::endl;
    std::cout << colors::BOLD_WHITE << "Options:" << colors::RESET << std::endl;
    std::cout << "  --nuke            " << colors::BOLD_RED << "Remove ALL installed packages and caches completely" << colors::RESET << std::endl;
    std::cout << "  --noconfirm       Skip confirmation prompts" << std::endl;
    std::cout << "  --verbose, -v     Enable verbose output" << std::endl;
    std::cout << "  --color=<when>    Color output (auto, always, never)" << std::endl;
    std::cout << "  --version         Show version information" << std::endl;
    std::cout << "  -h, --help        Show this help message" << std::endl;
    std::cout << std::endl;
}

} // namespace macman
