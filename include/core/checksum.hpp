// checksum.hpp — Shell-Free SHA-256 Verification Utility
// Provides a centralized, secure way to verify file integrity using
// native macOS shasum tool via posix_spawn. No shell pipes or popen.

#pragma once
#include <string>

namespace macman {

class Checksum {
public:
    /// Verify that the file at file_path matches the expected_sha256 string.
    /// Returns true if match, false otherwise.
    static bool verify_sha256(const std::string& file_path, const std::string& expected_sha256);

    /// Compute SHA-256 for a file and return the hash string.
    static std::string compute_sha256(const std::string& file_path);
};

} // namespace macman
