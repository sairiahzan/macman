// progress_bar.cpp — Pacman-Style Progress Bar Implementation
// Renders a terminal progress bar that mimics pacman:
// package-name     (42%) [########--------]  50.8 MiB / 110.3 MiB  2.5 MiB/s


#include "ui/progress_bar.hpp"
#include "cli/colors.hpp"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <sys/ioctl.h>
#include <unistd.h>

namespace macman {

// --- Constructor ---

ProgressBar::ProgressBar(const std::string& label, size_t total_bytes)
    : label_(label), total_bytes_(total_bytes),
      start_time_(std::chrono::steady_clock::now()),
      last_update_(std::chrono::steady_clock::now()) {}

// --- Update Progress ---

void ProgressBar::update(size_t current_bytes, double speed) {
    current_bytes_ = current_bytes;
    speed_ = speed;
    last_update_ = std::chrono::steady_clock::now();
    render();
}

// --- Mark as Finished ---

void ProgressBar::finish() {
    current_bytes_ = total_bytes_;
    complete_ = true;
    render();
    std::cout << std::endl; // Move to new line after completion
}

// --- Get Terminal Width ---

int ProgressBar::get_terminal_width() const {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
        return w.ws_col;
    }
    return 80; // Default fallback
}

// --- Format Bytes to Human-Readable ---

std::string ProgressBar::format_bytes(size_t bytes) const {
    const char* units[] = {"B", "KiB", "MiB", "GiB"};
    int idx = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024.0 && idx < 3) {
        size /= 1024.0;
        idx++;
    }

    std::ostringstream oss;
    if (idx == 0) {
        oss << bytes << " " << units[idx];
    } else {
        oss << std::fixed << std::setprecision(1) << size << " " << units[idx];
    }
    return oss.str();
}

// --- Format Speed ---

std::string ProgressBar::format_speed(double bytes_per_sec) const {
    if (bytes_per_sec <= 0) return "---";
    
    std::ostringstream oss;
    if (bytes_per_sec >= 1024 * 1024) {
        oss << std::fixed << std::setprecision(1) << (bytes_per_sec / (1024 * 1024)) << " MiB/s";
    } else if (bytes_per_sec >= 1024) {
        oss << std::fixed << std::setprecision(0) << (bytes_per_sec / 1024) << " KiB/s";
    } else {
        oss << std::fixed << std::setprecision(0) << bytes_per_sec << " B/s";
    }
    return oss.str();
}

// --- Build Bar String ---

std::string ProgressBar::build_bar_string(double percentage, int width) const {
    int filled = static_cast<int>(std::round(percentage * width / 100.0));
    filled = std::min(filled, width);
    
    std::string bar = "[";
    for (int i = 0; i < width; i++) {
        if (i < filled) {
            bar += "#";
        } else {
            bar += "-";
        }
    }
    bar += "]";
    
    return bar;
}

// --- Get Percentage ---

double ProgressBar::get_percentage() const {
    if (total_bytes_ == 0) return 0.0;
    return (static_cast<double>(current_bytes_) / static_cast<double>(total_bytes_)) * 100.0;
}

// --- Get Speed String ---

std::string ProgressBar::get_speed_string() const {
    return format_speed(speed_);
}

// --- Get ETA String ---

std::string ProgressBar::get_eta_string() const {
    if (speed_ <= 0 || current_bytes_ >= total_bytes_) return "";
    
    double remaining = static_cast<double>(total_bytes_ - current_bytes_);
    double eta_secs = remaining / speed_;
    
    int mins = static_cast<int>(eta_secs) / 60;
    int secs = static_cast<int>(eta_secs) % 60;
    
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(2) << mins << ":" 
        << std::setfill('0') << std::setw(2) << secs;
    return oss.str();
}

// --- Render the Progress Bar ---

void ProgressBar::render() {
    double pct = get_percentage();
    int term_width = get_terminal_width();
    
    // Truncate label to fit in 20 chars
    std::string display_label = label_;
    const int label_width = 20;
    if ((int)display_label.length() > label_width) {
        display_label = display_label.substr(0, label_width - 1) + "~";
    }
    while ((int)display_label.length() < label_width) {
        display_label += ' ';
    }

    // Build the components
    std::ostringstream percentage_str;
    percentage_str << "(" << std::setw(3) << std::fixed << std::setprecision(0) << pct << "%)";

    std::string current_str = format_bytes(current_bytes_);
    std::string total_str   = format_bytes(total_bytes_);
    std::string speed_str   = format_speed(speed_);
    
    std::string size_info = current_str + " / " + total_str;

    // Calculate available space for the bar
    // label(20) + space(1) + pct(6) + space(1) + space(1) + size_info + space(2) + speed
    int fixed_width = label_width + 1 + 6 + 1 + 1 + (int)size_info.length() + 2 + (int)speed_str.length();
    
    // Safely calculate remaining space for the progress bar
    int available = term_width - fixed_width - 8; // generous margin
    int actual_bar_width = std::max(5, std::min(available, 20)); // shorter bar as requested

    std::string bar = build_bar_string(pct, actual_bar_width);

    // Color the percentage
    const char* pct_color = colors::BOLD_WHITE;
    if (pct >= 100.0) pct_color = colors::BOLD_GREEN;
    else if (pct >= 50.0) pct_color = colors::BOLD_YELLOW;

    // Build final line
    std::cout << "\r\x1b[2K" 
              << colors::BOLD_WHITE << display_label
              << " " << pct_color << percentage_str.str()
              << " " << colors::BOLD_CYAN << bar
              << " " << colors::RESET << size_info
              << "  " << colors::DIM << speed_str
              << colors::RESET;
    
    std::cout.flush();
}

// --- MultiProgress Implementation ---

int MultiProgress::add_bar(const std::string& label, size_t total) {
    std::lock_guard<std::mutex> lock(render_mutex_);
    bars_.emplace_back(label, total);
    return static_cast<int>(bars_.size()) - 1;
}

void MultiProgress::update_bar(int id, size_t current, double speed) {
    std::lock_guard<std::mutex> lock(render_mutex_);
    if (id >= 0 && id < (int)bars_.size()) {
        bars_[id].update(current, speed);
    }
}

void MultiProgress::finish_bar(int id) {
    std::lock_guard<std::mutex> lock(render_mutex_);
    if (id >= 0 && id < (int)bars_.size()) {
        bars_[id].finish();
    }
}

void MultiProgress::render_all() {
    std::lock_guard<std::mutex> lock(render_mutex_);
    for (auto& bar : bars_) {
        bar.render();
        std::cout << std::endl;
    }
}

} // namespace macman
