/*
 * ============================================================================
 *  downloader.hpp — Multi-File Download Manager
 * ============================================================================
 *  High-level download manager that coordinates file downloads with
 *  caching, parallel execution, and integrated progress bar display.
 * ============================================================================
 */

#pragma once

#include "http_client.hpp"
#include "../ui/progress_bar.hpp"
#include <string>
#include <vector>
#include <queue>
#include <mutex>

namespace macman {

// ─── Download Task ──────────────────────────────────────────────────────────

struct DownloadTask {
    std::string url;            // URL to download from
    std::string output_path;    // Local file path to save to
    std::string label;          // Display name for progress bar
    size_t expected_size = 0;   // Expected file size (0 = unknown)
};

// ─── Download Result ────────────────────────────────────────────────────────

struct DownloadResult {
    std::string output_path;
    bool success = false;
    std::string error;
};

// ─── Downloader Class ───────────────────────────────────────────────────────

class Downloader {
public:
    explicit Downloader(size_t max_parallel = 4);
    ~Downloader() = default;

    // ─── Single File Download ───────────────────────────────────────────

    DownloadResult download(const DownloadTask& task);

    // ─── Batch Download ─────────────────────────────────────────────────

    std::vector<DownloadResult> download_all(const std::vector<DownloadTask>& tasks);

    // ─── Cache Management ───────────────────────────────────────────────

    bool is_cached(const std::string& filename) const;
    std::string get_cache_path(const std::string& filename) const;
    void clear_cache();
    size_t get_cache_size() const;

private:
    size_t max_parallel_;
    std::string cache_dir_;
    std::mutex progress_mutex_;
};

} // namespace macman
