// Arda Yiğit - Hazani
// logger.hpp — Centralized Thread-Safe Logging System
// Support levels: DEBUG, INFO, WARNING, ERROR.
// Writes to /usr/local/var/log/macman/macman.log (or configured path).

#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <iostream>

namespace macman {

enum class LogLevel {
    LVL_DEBUG,
    LVL_INFO,
    LVL_WARNING,
    LVL_ERROR
};

class Logger {
public:
    static Logger& instance();

    void init(const std::string& log_path);
    
    void log(LogLevel level, const std::string& message);
    
    // Convenience macros/methods
    void debug(const std::string& msg)   { log(LogLevel::LVL_DEBUG, msg); }
    void info(const std::string& msg)    { log(LogLevel::LVL_INFO, msg); }
    void warn(const std::string& msg)    { log(LogLevel::LVL_WARNING, msg); }
    void error(const std::string& msg)   { log(LogLevel::LVL_ERROR, msg); }

private:
    Logger() = default;
    ~Logger();

    std::string log_path_;
    std::ofstream log_file_;
    std::mutex mutex_;
    bool initialized_ = false;

    std::string level_to_string(LogLevel level);
    std::string get_timestamp();
};

} // namespace macman
