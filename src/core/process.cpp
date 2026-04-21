// process.cpp — Shell-Free Process Execution Implementation
// Uses posix_spawn to exec binaries directly without invoking /bin/sh.

#include "core/process.hpp"
#include <spawn.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

#ifdef __APPLE__
#  include <crt_externs.h>
#  define MACMAN_ENVIRON (*_NSGetEnviron())
#else
extern char** environ;
#  define MACMAN_ENVIRON environ
#endif

#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

namespace macman {

int run_exec_capturing(const std::string& binary,
                        const std::vector<std::string>& args,
                        std::string& output,
                        const std::string& cwd) {
    output.clear();
    
    // We use a temporary file to capture output. In a multi-threaded manager,
    // we should ensure this is thread-safe.
    std::string temp_log = "/tmp/macman_spawn_" + std::to_string(getpid()) + "_" + std::to_string(rand()) + ".log";

    // Build argv
    std::vector<std::string> argv_store;
    argv_store.push_back(binary.substr(binary.rfind('/') + 1));
    for (const auto& a : args) argv_store.push_back(a);

    std::vector<char*> c_argv;
    for (auto& s : argv_store) c_argv.push_back(const_cast<char*>(s.c_str()));
    c_argv.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, temp_log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, STDOUT_FILENO, STDERR_FILENO);

    if (!cwd.empty()) {
        posix_spawn_file_actions_addchdir(&fa, cwd.c_str());
    }

    pid_t pid;
    int rc = posix_spawn(&pid, binary.c_str(), &fa, nullptr, c_argv.data(), MACMAN_ENVIRON);
    posix_spawn_file_actions_destroy(&fa);

    if (rc == 0) {
        int ws;
        waitpid(pid, &ws, 0);
        rc = WIFEXITED(ws) ? WEXITSTATUS(ws) : -1;
        
        // Read the output
        std::ifstream f(temp_log);
        if (f.is_open()) {
            output.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }
    } else {
        rc = -1;
    }

    fs::remove(temp_log);
    return rc;
}

int run_exec(const std::string& binary,
             const std::vector<std::string>& args,
             bool quiet,
             const std::string& cwd) {
    // Build argv: [basename(binary), args..., nullptr]
    std::vector<std::string> argv_store;
    argv_store.reserve(args.size() + 1);

    // argv[0] = basename of binary
    std::string base = binary;
    auto pos = base.rfind('/');
    if (pos != std::string::npos) base = base.substr(pos + 1);
    argv_store.push_back(std::move(base));

    for (const auto& a : args)
        argv_store.push_back(a);

    // Build C-style argv array
    std::vector<char*> c_argv;
    c_argv.reserve(argv_store.size() + 1);
    for (auto& s : argv_store)
        c_argv.push_back(const_cast<char*>(s.c_str()));
    c_argv.push_back(nullptr);

    // Configure file actions (output suppression, working directory)
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);

    if (quiet) {
        posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO,
                                          "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_adddup2(&fa, STDOUT_FILENO, STDERR_FILENO);
    }

    if (!cwd.empty()) {
        posix_spawn_file_actions_addchdir(&fa, cwd.c_str());
    }

    // Spawn the process directly — no shell involved
    pid_t pid;
    int rc = posix_spawn(&pid, binary.c_str(), &fa, nullptr,
                          c_argv.data(), MACMAN_ENVIRON);
    posix_spawn_file_actions_destroy(&fa);

    if (rc != 0) return -1;

    // Wait for completion and return exit code
    int ws;
    waitpid(pid, &ws, 0);
    return WIFEXITED(ws) ? WEXITSTATUS(ws) : -1;
}

} // namespace macman
