// process.hpp — Shell-Free Process Execution Utilities
// Replaces all system() calls with direct posix_spawn binary execution.

#pragma once
#include <string>
#include <vector>

namespace macman {

/// Direct binary execution — no shell spawned.
///
/// @param binary  Absolute path to the executable (e.g. "/usr/bin/tar").
/// @param args    Argument list (argv[1..N]; argv[0] is derived from binary basename).
/// @param quiet   If true, stdout and stderr are redirected to /dev/null.
/// @param cwd     If non-empty, the child process chdir()s here before exec.
/// @return Exit code (0 = success), or -1 on spawn failure.
int run_exec(const std::string& binary,
             const std::vector<std::string>& args,
             bool quiet = true,
             const std::string& cwd = "");

} // namespace macman
