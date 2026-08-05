#pragma once

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <functional>
#include <nlohmann/json.hpp>
#include "../utils/error.h"

namespace goose {

struct HttpResponse {
    int status_code = 0;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
};

// Thread-safe line stream for async SSE requests. The worker thread performs
// the HTTP request and pushes parsed lines; consumers block on pop_line().
struct SseStream {
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::string buffer;
    std::vector<std::string> lines;
    bool done = false;
    bool error = false;
    std::string error_message;
    int status_code = 0;
    std::thread worker;

    ~SseStream();

    // Blocks until a line is available or the stream is finished.
    // Returns false when the stream is finished and drained.
    bool pop_line(std::string& line);
    bool has_lines() const;
};

using SseStreamPtr = std::shared_ptr<SseStream>;

class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    void set_auth_token(const std::string& token);
    void set_extra_headers(const std::vector<std::string>& headers);

    Result<HttpResponse> post_json(const std::string& url, const nlohmann::json& body);

    // Starts the request on a worker thread and returns immediately.
    // The stream is joined (blocking) when the last line is consumed or on destruction.
    SseStreamPtr post_json_sse_async(const std::string& url, const nlohmann::json& body);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace goose
