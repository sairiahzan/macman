// checksum.cpp — Shell-Free SHA-256 Implementation

#include "core/checksum.hpp"
#include "core/process.hpp"
#include "cli/colors.hpp"
#include <sstream>
#include <algorithm>

namespace macman {

std::string Checksum::compute_sha256(const std::string& file_path) {
    std::string output;
    // Run /usr/bin/shasum -a 256 <file_path>
    int rc = run_exec_capturing("/usr/bin/shasum", {"-a", "256", file_path}, output);
    
    if (rc != 0 || output.empty()) {
        return "";
    }

    // shasum output format: "hash  filename\n"
    std::istringstream ss(output);
    std::string hash;
    ss >> hash;
    return hash;
}

bool Checksum::verify_sha256(const std::string& file_path, const std::string& expected_sha256) {
    if (expected_sha256.empty()) return true;

    std::string actual_sha = compute_sha256(file_path);
    
    if (actual_sha == expected_sha256) {
        return true;
    }

    colors::print_error("SHA-256 verification failed for: " + file_path);
    colors::print_error("Expected: " + expected_sha256);
    colors::print_error("Got:      " + actual_sha);
    return false;
}

} // namespace macman
