// Arda Yiğit - Hazani
// progress_bar.hpp — Pacman-Style Progress Bar Renderer

#pragma once

#include <string>
#include <chrono>
#include <mutex>
#include <vector>

namespace macman {

class ProgressBar {
public:
    ProgressBar(const std::string& label, size_t total_bytes);
    ~ProgressBar() = default;

    void update(size_t current_bytes, double speed = 0.0);
    void update_no_render(size_t current_bytes, double speed = 0.0);
    void finish();
    void finish_no_render();
    void render();

    void set_label(const std::string& label) { label_ = label; }
    void set_total(size_t total) { total_bytes_ = total; }
    void set_bar_width(int width) { bar_width_ = width; }
    
    bool is_complete() const { return complete_; }
    double get_percentage() const;
    std::string get_speed_string() const;
    std::string get_eta_string() const;

private:
    std::string label_;
    size_t total_bytes_;
    size_t current_bytes_ = 0;
    double speed_ = 0.0;
    int bar_width_ = 25;
    bool complete_ = false;
    
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point last_update_;

    int get_terminal_width() const;
    std::string format_bytes(size_t bytes) const;
    std::string format_speed(double bytes_per_sec) const;
    std::string build_bar_string(double percentage, int width) const;
};

class MultiProgress {
public:
    MultiProgress() = default;
    ~MultiProgress() = default;

    int add_bar(const std::string& label, size_t total);
    void update_bar(int id, size_t current, double speed = 0.0);
    void finish_bar(int id);
    void render_all_at_once();

private:
    std::vector<ProgressBar> bars_;
    std::mutex render_mutex_;
    bool first_render_ = true;
};

} // namespace macman
