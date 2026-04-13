// http_client.hpp — libcurl HTTP Client Wrapper
// Thin C++ wrapper around libcurl providing simple GET requests, file
// downloads with progress callbacks, connection reuse, and error handling.


#pragma once

#include <string>
#include <functional>
#include <curl/curl.h>

namespace macman {

// --- Progress Callback Type ---
// Parameters: total_bytes, downloaded_bytes, speed_bytes_per_sec
using ProgressCallback = std::function<void(size_t, size_t, double)>;

// --- HTTP Response ---

struct HttpResponse {
    long status_code = 0;
    std::string body;
    std::string error;
    bool success = false;
};

// --- HTTP Client Class ---

class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    // Non-copyable, movable
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // --- HTTP Methods ---

    HttpResponse get(const std::string& url);
    HttpResponse get_json(const std::string& url);
    
    // --- File Download ---

    bool download_file(const std::string& url, 
                       const std::string& output_path,
                       ProgressCallback progress_cb = nullptr);

    // --- Configuration ---

    void set_timeout(int seconds);
    void set_user_agent(const std::string& ua);

private:
    CURL* curl_handle_;
    int timeout_ = 30;
    std::string user_agent_;

    // --- CURL Callbacks ---

    static size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata);
    static size_t file_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata);
    static int progress_callback_wrapper(void* clientp, curl_off_t dltotal, 
                                          curl_off_t dlnow, curl_off_t ultotal, 
                                          curl_off_t ulnow);

    void setup_common(CURL* handle, const std::string& url);
};

// --- Global Init / Cleanup ---
// Call once at program startup/shutdown

void http_global_init();
void http_global_cleanup();

} // namespace macman
