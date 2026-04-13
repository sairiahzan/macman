/*
 * ============================================================================
 *  colors.hpp — ANSI Color Codes and Terminal Formatting Utilities
 * ============================================================================
 *  Provides pacman-style terminal output formatting with colored text,
 *  bold labels, status indicators, and structured message helpers.
 * ============================================================================
 */

#pragma once

#include <string>
#include <iostream>

namespace macman::colors {

// ─── ANSI Escape Codes ──────────────────────────────────────────────────────

constexpr const char* RESET      = "\033[0m";
constexpr const char* BOLD       = "\033[1m";
constexpr const char* DIM        = "\033[2m";
constexpr const char* UNDERLINE  = "\033[4m";

// ─── Foreground Colors ──────────────────────────────────────────────────────

constexpr const char* RED        = "\033[31m";
constexpr const char* GREEN      = "\033[32m";
constexpr const char* YELLOW     = "\033[33m";
constexpr const char* BLUE       = "\033[34m";
constexpr const char* MAGENTA    = "\033[35m";
constexpr const char* CYAN       = "\033[36m";
constexpr const char* WHITE      = "\033[37m";

// ─── Bold + Color Combinations ──────────────────────────────────────────────

constexpr const char* BOLD_RED     = "\033[1;31m";
constexpr const char* BOLD_GREEN   = "\033[1;32m";
constexpr const char* BOLD_YELLOW  = "\033[1;33m";
constexpr const char* BOLD_BLUE    = "\033[1;34m";
constexpr const char* BOLD_MAGENTA = "\033[1;35m";
constexpr const char* BOLD_CYAN    = "\033[1;36m";
constexpr const char* BOLD_WHITE   = "\033[1;37m";

// ─── Pacman-Style Prefix Markers ────────────────────────────────────────────

// ":: " prefix used in pacman for prompts and section headers
inline void print_action(const std::string& msg) {
    std::cout << BOLD_BLUE << ":: " << BOLD_WHITE << msg << RESET << std::endl;
}

// "==> " prefix used for primary status messages
inline void print_status(const std::string& msg) {
    std::cout << BOLD_GREEN << "==> " << BOLD_WHITE << msg << RESET << std::endl;
}

// " -> " prefix used for secondary/sub-status messages
inline void print_substatus(const std::string& msg) {
    std::cout << BOLD_BLUE << " -> " << BOLD_WHITE << msg << RESET << std::endl;
}

// ─── Message Type Helpers ───────────────────────────────────────────────────

inline void print_error(const std::string& msg) {
    std::cerr << BOLD_RED << "error: " << RESET << msg << std::endl;
}

inline void print_warning(const std::string& msg) {
    std::cerr << BOLD_YELLOW << "warning: " << RESET << msg << std::endl;
}

inline void print_success(const std::string& msg) {
    std::cout << BOLD_GREEN << "success: " << RESET << msg << std::endl;
}

inline void print_debug(const std::string& msg) {
    std::cerr << DIM << "  ⟫ " << CYAN << msg << RESET << std::endl;
}

// ─── Inline Formatting Functions ────────────────────────────────────────────

inline std::string bold(const std::string& text) {
    return std::string(BOLD) + text + RESET;
}

inline std::string colorize(const char* color, const std::string& text) {
    return std::string(color) + text + RESET;
}

inline std::string pkg_name(const std::string& name) {
    return std::string(BOLD_WHITE) + name + RESET;
}

inline std::string pkg_version(const std::string& ver) {
    return std::string(BOLD_GREEN) + ver + RESET;
}

inline std::string pkg_size(const std::string& size) {
    return std::string(BOLD_CYAN) + size + RESET;
}

} // namespace macman::colors
