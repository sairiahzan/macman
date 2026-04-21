// http_client.cpp — libcurl HTTP Client Implementation [V1.2.0 Patch]
// Implements HTTP GET, JSON fetching, and file downloads using libcurl.
// Provides connection reuse, progress callbacks, and proper error handling.


#include "net/http_client.hpp"
#include "macman.hpp"
#include <fstream>
#include <chrono>
#include <cstring>

namespace macman {

// --- Callback Data Structs ---

struct WriteData {
    std::string* response;
};

struct FileWriteData {
    std::ofstream* file;
};

struct ProgressData {
    ProgressCallback* callback;
    std::chrono::steady_clock::time_point start_time;
};

// --- Global Init / Cleanup ---

void http_global_init() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

void http_global_cleanup() {
    curl_global_cleanup();
}

// --- Constructor / Destructor ---

HttpClient::HttpClient() 
    : curl_handle_(nullptr), 
      timeout_(HTTP_TIMEOUT_SECONDS),
      user_agent_(std::string(PROGRAM_NAME) + "/" + VERSION) {
    curl_handle_ = curl_easy_init();
}

HttpClient::~HttpClient() {
    if (curl_handle_) {
        curl_easy_cleanup(curl_handle_);
    }
}

// --- Common CURL Setup ---

void HttpClient::setup_common(CURL* handle, const std::string& url) {
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_USERAGENT, user_agent_.c_str());
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, (long)timeout_);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, ""); // Accept all encodings
}

// --- CURL Write Callback (String) ---

size_t HttpClient::write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t total_size = size * nmemb;
    auto* data = static_cast<WriteData*>(userdata);
    data->response->append(ptr, total_size);
    return total_size;
}

// --- CURL Write Callback (File) ---

size_t HttpClient::file_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t total_size = size * nmemb;
    auto* data = static_cast<FileWriteData*>(userdata);
    data->file->write(ptr, total_size);
    return data->file->good() ? total_size : 0;
}

// --- CURL Progress Callback ---

int HttpClient::progress_callback_wrapper(void* clientp, curl_off_t dltotal,
                                           curl_off_t dlnow, curl_off_t /*ultotal*/,
                                           curl_off_t /*ulnow*/) {
    auto* data = static_cast<ProgressData*>(clientp);
    if (data->callback && *data->callback && dltotal > 0) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double>(now - data->start_time).count();
        double speed = (elapsed > 0) ? static_cast<double>(dlnow) / elapsed : 0.0;
        
        (*data->callback)(static_cast<size_t>(dltotal), 
                         static_cast<size_t>(dlnow), 
                         speed);
    }
    return 0; // 0 = continue, 1 = abort
}

// --- GET Request ---

HttpResponse HttpClient::get(const std::string& url) {
    HttpResponse response;

    CURL* handle = curl_easy_init();
    if (!handle) {
        response.error = "Failed to initialize CURL";
        return response;
    }

    WriteData write_data{&response.body};

    setup_common(handle, url);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &write_data);

    CURLcode result = curl_easy_perform(handle);
    
    if (result == CURLE_OK) {
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response.status_code);
        response.success = (response.status_code >= 200 && response.status_code < 300);
    } else {
        response.error = curl_easy_strerror(result);
        response.success = false;
    }

    curl_easy_cleanup(handle);
    return response;
}

// --- GET JSON ---

HttpResponse HttpClient::get_json(const std::string& url) {
    HttpResponse response;

    CURL* handle = curl_easy_init();
    if (!handle) {
        response.error = "Failed to initialize CURL";
        return response;
    }

    WriteData write_data{&response.body};

    setup_common(handle, url);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &write_data);

    // Set JSON accept header
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json");
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);

    CURLcode result = curl_easy_perform(handle);
    
    if (result == CURLE_OK) {
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response.status_code);
        response.success = (response.status_code >= 200 && response.status_code < 300);
    } else {
        response.error = curl_easy_strerror(result);
        response.success = false;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(handle);
    return response;
}

size_t HttpClient::get_file_size(const std::string& url) {
    CURL* handle = curl_easy_init();
    if (!handle) return 0;

    setup_common(handle, url);
    curl_easy_setopt(handle, CURLOPT_NOBODY, 1L); // HEAD request
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);

    struct curl_slist* headers = nullptr;

    // Handle GHCR Authentication for Homebrew bottles
    if (url.find("ghcr.io/v2/") != std::string::npos) {
        size_t v2_pos = url.find("ghcr.io/v2/");
        size_t repo_start = v2_pos + 11;
        size_t blobs_pos = url.find("/blobs/", repo_start);

        if (blobs_pos != std::string::npos) {
            std::string repo = url.substr(repo_start, blobs_pos - repo_start);
            std::string token_url = "https://ghcr.io/token?service=ghcr.io&scope=repository:" + repo + ":pull";

            auto auth_resp = get_json(token_url);
            if (auth_resp.success) {
                try {
                    size_t tok_pos = auth_resp.body.find("\"token\":\"");
                    if (tok_pos != std::string::npos) {
                        tok_pos += 9;
                        size_t end_pos = auth_resp.body.find("\"", tok_pos);
                        if (end_pos != std::string::npos) {
                            std::string token = auth_resp.body.substr(tok_pos, end_pos - tok_pos);
                            std::string auth_header = "Authorization: Bearer " + token;
                            headers = curl_slist_append(headers, auth_header.c_str());
                            curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
                        }
                    }
                } catch (...) {}
            }
        }
    }

    double file_size = 0.0;
    CURLcode result = curl_easy_perform(handle);
    
    if (result == CURLE_OK) {
        curl_easy_getinfo(handle, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &file_size);
    }
    
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(handle);
    return file_size > 0.0 ? static_cast<size_t>(file_size) : 0;
}

// --- Download File with Progress ---

HttpResponse HttpClient::download_file(const std::string& url, 
                                        const std::string& output_path,
                                        ProgressCallback progress_cb) {
    HttpResponse response;
    CURL* handle = curl_easy_init();
    if (!handle) {
        response.error = "Failed to initialize CURL";
        response.success = false;
        return response;
    }

    std::ofstream file(output_path, std::ios::binary);
    if (!file.is_open()) {
        curl_easy_cleanup(handle);
        response.error = "Could not open output file: " + output_path;
        response.success = false;
        return response;
    }

    FileWriteData file_data{&file};
    ProgressData progress_data{&progress_cb, std::chrono::steady_clock::now()};

    setup_common(handle, url);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, (long)DOWNLOAD_TIMEOUT_SECS);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, file_write_callback);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &file_data);

    // Enable progress callback
    if (progress_cb) {
        curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, progress_callback_wrapper);
        curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &progress_data);
    }

    struct curl_slist* headers = nullptr;

    // Handle GHCR Authentication for Homebrew bottles
    if (url.find("ghcr.io/v2/") != std::string::npos) {
        // Extract repo name: https://ghcr.io/v2/homebrew/core/ncurses/blobs/...
        size_t v2_pos = url.find("ghcr.io/v2/");
        size_t repo_start = v2_pos + 11;
        size_t blobs_pos = url.find("/blobs/", repo_start);
        
        if (blobs_pos != std::string::npos) {
            std::string repo = url.substr(repo_start, blobs_pos - repo_start);
            std::string token_url = "https://ghcr.io/token?service=ghcr.io&scope=repository:" + repo + ":pull";
            
            // Re-entrant call to get JSON
            auto auth_resp = get_json(token_url);
            if (auth_resp.success) {
                try {
                    // Quick string parse to avoid nlohmann/json.hpp dependency here, 
                    // or just use direct string search since it's a simple flat JSON: {"token":"..."}
                    size_t tok_pos = auth_resp.body.find("\"token\":\"");
                    if (tok_pos != std::string::npos) {
                        tok_pos += 9;
                        size_t end_pos = auth_resp.body.find("\"", tok_pos);
                        if (end_pos != std::string::npos) {
                            std::string token = auth_resp.body.substr(tok_pos, end_pos - tok_pos);
                            std::string auth_header = "Authorization: Bearer " + token;
                            headers = curl_slist_append(headers, auth_header.c_str());
                            curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
                        }
                    }
                } catch (...) {}
            }
        }
    }

    CURLcode result = curl_easy_perform(handle);
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response.status_code);
    file.close();

    response.success = (result == CURLE_OK && response.status_code >= 200 && response.status_code < 300);
    if (!response.success) {
        std::remove(output_path.c_str());
        if (result != CURLE_OK) {
            response.error = "cURL error: " + std::string(curl_easy_strerror(result));
        } else {
            response.error = "HTTP HTTP status: " + std::to_string(response.status_code);
        }
    }

    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(handle);
    return response;
}

// --- Configuration ---

void HttpClient::set_timeout(int seconds) {
    timeout_ = seconds;
}

void HttpClient::set_user_agent(const std::string& ua) {
    user_agent_ = ua;
}

} // namespace macman
