// Arda Yiğit - Hazani
// config.hpp — Configuration File Manager
// Manages the macman configuration file (/usr/local/etc/macman.conf).
// Provides read/write access to user-configurable settings like cache
// directory, parallel download count, color output, and logging.


#pragma once

#include <string>
#include <map>

namespace macman {

class Config {
public:
    // --- Singleton Access ---

    static Config& instance();

    // --- Load / Save ---

    bool load(const std::string& path = "");
    bool save(const std::string& path = "") const;
    void create_default(const std::string& path = "") const;

    // --- Getters ---

    std::string get_cache_dir() const;
    std::string get_db_dir() const;
    std::string get_log_file() const;
    int         get_parallel_downloads() const;
    bool        get_color_enabled() const;
    bool        get_verbose() const;
    std::string get(const std::string& key, const std::string& default_val = "") const;

    // --- Setters ---

    void set(const std::string& key, const std::string& value);

private:
    Config() = default;
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

    std::map<std::string, std::string> settings_;
    std::string config_path_;

    void set_defaults();
};

} // namespace macman
