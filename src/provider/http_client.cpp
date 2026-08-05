#include "provider/http_client.h"
#include <curl/curl.h>
#include <spdlog/spdlog.h>

namespace goose {

namespace {

// libcurl requires curl_global_init before any handle is created; safe to call
// once from any thread (C++11 magic statics are thread-safe).
void ensure_curl_global_init() {
    static const bool initialized = [] {
        curl_global_init(CURL_GLOBAL_ALL);
        return true;
    }();
    (void)initialized;
}

// Each request uses its own CURL handle; never share a handle across threads.
CURL* new_curl_handle() {
    ensure_curl_global_init();
    CURL* curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
    }
    return curl;
}

} // namespace

struct HttpClient::Impl {
    std::string auth_token;
    std::vector<std::string> extra_headers;

    struct curl_slist* build_headers(const std::vector<std::string>& extra) {
        struct curl_slist* headers = nullptr;
        if (!auth_token.empty()) {
            std::string h = "Authorization: Bearer " + auth_token;
            headers = curl_slist_append(headers, h.c_str());
        }
        for (const auto& h : extra_headers) {
            headers = curl_slist_append(headers, h.c_str());
        }
        for (const auto& h : extra) {
            headers = curl_slist_append(headers, h.c_str());
        }
        return headers;
    }
};

static size_t write_callback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

HttpClient::HttpClient() : impl_(std::make_shared<Impl>()) {}
HttpClient::~HttpClient() = default;

void HttpClient::set_auth_token(const std::string& token) { impl_->auth_token = token; }
void HttpClient::set_extra_headers(const std::vector<std::string>& headers) {
    impl_->extra_headers = headers;
}

Result<HttpResponse> HttpClient::post_json(const std::string& url, const nlohmann::json& body) {
    CURL* curl = new_curl_handle();
    if (!curl) {
        return Result<HttpResponse>::err(make_error(
            ErrorCode::NetworkError, "curl: failed to initialize"));
    }
    HttpResponse response;
    std::string body_str = body.dump();

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body_str.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);

    auto* headers = impl_->build_headers({"Content-Type: application/json"});
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        return Result<HttpResponse>::err(make_error(
            ErrorCode::NetworkError, std::string("curl: ") + curl_easy_strerror(res)));
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code);
    curl_easy_cleanup(curl);
    return Result<HttpResponse>::ok(std::move(response));
}

SseStream::~SseStream() {
    if (worker.joinable()) {
        // The worker holds the last shared_ptr when the consumer has already
        // released its reference, so destruction can run on the worker thread
        // itself. Joining yourself is EDEADLK; detach instead (the worker body
        // has finished by the time its captured reference is released).
        if (worker.get_id() == std::this_thread::get_id()) {
            worker.detach();
        } else {
            worker.join();
        }
    }
}

bool SseStream::has_lines() const {
    std::lock_guard<std::mutex> lock(mutex);
    return !lines.empty();
}

bool SseStream::pop_line(std::string& line) {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [this]() { return done || !lines.empty(); });
    if (lines.empty()) {
        return false;
    }
    line = std::move(lines.front());
    lines.erase(lines.begin());
    return true;
}

static size_t sse_write_callback_async(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* sse = static_cast<SseStream*>(userp);
    size_t total = size * nmemb;

    {
        std::lock_guard<std::mutex> lock(sse->mutex);
        sse->buffer.append(static_cast<char*>(contents), total);

        size_t pos;
        while ((pos = sse->buffer.find('\n')) != std::string::npos) {
            std::string line = sse->buffer.substr(0, pos);
            sse->buffer = sse->buffer.substr(pos + 1);

            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (!line.empty()) {
                sse->lines.push_back(std::move(line));
            }
        }
    }

    sse->cv.notify_all();
    return total;
}

SseStreamPtr HttpClient::post_json_sse_async(const std::string& url, const nlohmann::json& body) {
    auto sse = std::make_shared<SseStream>();
    std::string body_str = body.dump();
    spdlog::debug("[sse] url={} body_size={} body_prefix={}", url, body_str.size(),
                  body_str.substr(0, 80));

    // Hold a shared reference to the impl so the worker never touches a freed
    // HttpClient even if the provider is destroyed mid-stream.
    std::shared_ptr<Impl> impl = impl_;

    sse->worker = std::thread([sse, impl, url, body_str]() {
        CURL* curl = nullptr;
        try {
            // An exception escaping this thread would abort the whole process,
            // so every error path must land in sse->error instead.
            curl = new_curl_handle();
            if (!curl) {
                std::lock_guard<std::mutex> lock(sse->mutex);
                sse->error = true;
                sse->error_message = "curl: failed to initialize";
                sse->done = true;
                sse->cv.notify_all();
                return;
            }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body_str.size());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sse_write_callback_async);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, sse.get());
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 120L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);

        auto* headers = impl->build_headers(
            {"Content-Type: application/json", "Accept: text/event-stream"});
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);

        {
            std::lock_guard<std::mutex> lock(sse->mutex);
            // Any residual data without a trailing newline is an unterminated
            // frame; dropping it keeps consumers from parsing a partial event
            // as a complete JSON document.
            if (!sse->buffer.empty()) {
                spdlog::warn("[sse] dropping unterminated trailing frame ({} bytes)",
                             sse->buffer.size());
                sse->buffer.clear();
            }
            if (res != CURLE_OK) {
                sse->error = true;
                sse->error_message = std::string("curl: ") + curl_easy_strerror(res);
            } else {
                long status_code = 0;
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
                sse->status_code = static_cast<int>(status_code);
            }
            sse->done = true;
        }
        curl_easy_cleanup(curl);
        sse->cv.notify_all();
        } catch (const std::exception& e) {
            spdlog::error("[sse] worker exception: {}", e.what());
            std::lock_guard<std::mutex> lock(sse->mutex);
            sse->error = true;
            sse->error_message = std::string("SSE worker failed: ") + e.what();
            sse->done = true;
            sse->cv.notify_all();
            if (curl) curl_easy_cleanup(curl);
        }
    });

    return sse;
}

} // namespace goose
