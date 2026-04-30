#include "core/logger.hpp"
#include <chrono>
#include <iomanip>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

namespace macman {

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

Logger::~Logger() {
    if (log_file_.is_open()) {
        log_file_.close();
    }
}

void Logger::init(const std::string& log_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) return;

    log_path_ = log_path;
    
    // Ensure directory exists
    try {
        fs::path p(log_path_);
        if (p.has_parent_path()) {
            fs::create_directories(p.parent_path());
        }
    } catch (...) {}

    log_file_.open(log_path_, std::ios::app);
    initialized_ = true;
}

void Logger::log(LogLevel level, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::string timestamp = get_timestamp();
    std::string level_str = level_to_string(level);
    
    std::string log_entry = "[" + timestamp + "] [" + level_str + "] " + message;

    // Write to file
    if (log_file_.is_open()) {
        log_file_ << log_entry << std::endl;
    }

    // Also write errors and warnings to cerr if in verbose mode?
    // For now, let's keep it in the file primarily.
}

std::string Logger::level_to_string(LogLevel level) {
    switch (level) {
        case LogLevel::LVL_DEBUG:   return "DEBUG";
        case LogLevel::LVL_INFO:    return "INFO";
        case LogLevel::LVL_WARNING: return "WARN";
        case LogLevel::LVL_ERROR:   return "ERROR";
        default:                    return "UNKNOWN";
    }
}

std::string Logger::get_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %X");
    return ss.str();
}

} // namespace macman
