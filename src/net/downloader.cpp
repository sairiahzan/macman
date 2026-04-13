// downloader.cpp — Multi-File Download Manager Implementation
// Coordinates downloads with caching, progress bar integration, and
// sequential/parallel execution modes.


#include "net/downloader.hpp"
#include "macman.hpp"
#include "core/config.hpp"
#include "cli/colors.hpp"
#include <filesystem>
#include <algorithm>
#include <numeric>

namespace fs = std::filesystem;

namespace macman {

// --- Constructor ---

Downloader::Downloader(size_t max_parallel)
    : max_parallel_(max_parallel),
      cache_dir_(Config::instance().get_cache_dir()) {
    // Ensure cache directory exists (best-effort, may fail without sudo)
    try {
        if (!fs::exists(cache_dir_)) {
            fs::create_directories(cache_dir_);
        }
    } catch (...) {}
}

// --- Single File Download ---

DownloadResult Downloader::download(const DownloadTask& task) {
    DownloadResult result;
    result.output_path = task.output_path;

    // Check if already cached
    std::string filename = fs::path(task.output_path).filename().string();
    if (is_cached(filename)) {
        std::string cached = get_cache_path(filename);
        try {
            fs::copy_file(cached, task.output_path, fs::copy_options::overwrite_existing);
            result.success = true;
            colors::print_substatus("Using cached: " + filename);
            return result;
        } catch (...) {
            // Cache copy failed — download fresh
        }
    }

    // Create progress bar
    ProgressBar progress(task.label, task.expected_size);

    // Download with progress callback
    HttpClient http;
    http.set_timeout(DOWNLOAD_TIMEOUT_SECS);

    bool success = http.download_file(task.url, task.output_path,
        [&progress](size_t total, size_t current, double speed) {
            if (total > 0) {
                progress.set_total(total);
            }
            progress.update(current, speed);
        });

    if (success) {
        progress.finish();
        
        // Cache the downloaded file
        try {
            std::string cache_path = get_cache_path(filename);
            fs::copy_file(task.output_path, cache_path, fs::copy_options::overwrite_existing);
        } catch (...) {
            // Caching is best-effort, don't fail on it
        }
        
        result.success = true;
    } else {
        std::cout << std::endl; // New line after failed progress bar
        result.error = "Download failed: " + task.url;
        result.success = false;
    }

    return result;
}

// --- Batch Download ---

std::vector<DownloadResult> Downloader::download_all(const std::vector<DownloadTask>& tasks) {
    std::vector<DownloadResult> results;
    results.reserve(tasks.size());

    // For now, download sequentially with progress bars
    // TODO: Add parallel download support with multi-progress display
    for (const auto& task : tasks) {
        results.push_back(download(task));
    }

    return results;
}

// --- Cache Checks ---

bool Downloader::is_cached(const std::string& filename) const {
    return fs::exists(get_cache_path(filename));
}

std::string Downloader::get_cache_path(const std::string& filename) const {
    return (fs::path(cache_dir_) / filename).string();
}

// --- Clear Cache ---

void Downloader::clear_cache() {
    try {
        for (const auto& entry : fs::directory_iterator(cache_dir_)) {
            fs::remove(entry.path());
        }
    } catch (const std::exception& e) {
        colors::print_error("Failed to clear cache: " + std::string(e.what()));
    }
}

// --- Get Cache Size ---

size_t Downloader::get_cache_size() const {
    size_t total = 0;
    try {
        for (const auto& entry : fs::directory_iterator(cache_dir_)) {
            if (entry.is_regular_file()) {
                total += entry.file_size();
            }
        }
    } catch (...) {}
    return total;
}

} // namespace macman
