// progress_bar.hpp — Pacman-Style Progress Bar Renderer
// Renders a terminal progress bar that mimics Arch Linux pacman's output:
// package-name    (42%) [########--------]  50.8 MiB / 110.3 MiB  2.5 MiB/s
// Supports terminal width detection, speed/ETA calculation, and animation.


#pragma once

#include <string>
#include <chrono>
#include <mutex>

namespace macman {

class ProgressBar {
public:
    // --- Constructor ---

    ProgressBar(const std::string& label, size_t total_bytes);
    ~ProgressBar() = default;

    // --- Update State ---

    void update(size_t current_bytes, double speed = 0.0);
    void finish();

    // --- Rendering ---

    void render();

    // --- Configuration ---

    void set_label(const std::string& label) { label_ = label; }
    void set_total(size_t total) { total_bytes_ = total; }
    void set_bar_width(int width) { bar_width_ = width; }
    
    // --- State Queries ---

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

    // --- Formatting Helpers ---

    int get_terminal_width() const;
    std::string format_bytes(size_t bytes) const;
    std::string format_speed(double bytes_per_sec) const;
    std::string build_bar_string(double percentage, int width) const;
};

// --- Multi-Progress Display ---
// Manages multiple concurrent progress bars (for parallel downloads)

class MultiProgress {
public:
    MultiProgress() = default;
    ~MultiProgress() = default;

    int add_bar(const std::string& label, size_t total);
    void update_bar(int id, size_t current, double speed = 0.0);
    void finish_bar(int id);
    void render_all();

private:
    std::vector<ProgressBar> bars_;
    std::mutex render_mutex_;
};

} // namespace macman
