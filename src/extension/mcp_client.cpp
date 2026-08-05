#include "extension/mcp_client.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>

#ifdef _WIN32
    #include <windows.h>
    #include <io.h>
    #define CLOSE_HANDLE(h) CloseHandle(h)
    #define INVALID_HANDLE_VAL INVALID_HANDLE_VALUE
    using fd_t = HANDLE;
    static constexpr fd_t FD_NONE = INVALID_HANDLE_VALUE;
#else
    #include <sys/wait.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <sys/select.h>
    #include <cerrno>
    #define CLOSE_HANDLE(h) ::close(h)
    using fd_t = int;
    static constexpr fd_t FD_NONE = -1;
#endif

namespace goose {

struct McpClient::Impl {
    std::string cmd;
    std::vector<std::string> args;
    std::unordered_map<std::string, std::string> envs;

    fd_t stdin_fd = FD_NONE;
    fd_t stdout_fd = FD_NONE;
    fd_t stderr_fd = FD_NONE;
#ifdef _WIN32
    HANDLE child_process = nullptr;
    HANDLE child_thread = nullptr;
#else
    pid_t child_pid = -1;
#endif
    bool connected = false;
    int request_id = 0;
    std::thread stderr_drain;

    ~Impl() {
        if (connected) {
            shutdown_impl(true);
        }
    }

    void shutdown() {
        shutdown_impl(false);
    }

    void shutdown_impl(bool from_destructor) {
#ifdef _WIN32
        if (child_process) {
            TerminateProcess(child_process, 0);
            if (!from_destructor) {
                WaitForSingleObject(child_process, 5000);
            }
            CloseHandle(child_process);
            child_process = nullptr;
        }
        if (child_thread) { CloseHandle(child_thread); child_thread = nullptr; }
        if (stdin_fd != FD_NONE) { CloseHandle(stdin_fd); stdin_fd = FD_NONE; }
        if (stdout_fd != FD_NONE) { CloseHandle(stdout_fd); stdout_fd = FD_NONE; }
        if (stderr_fd != FD_NONE) { CloseHandle(stderr_fd); stderr_fd = FD_NONE; }
#else
        if (child_pid > 0) {
            kill(child_pid, SIGTERM);
            int status;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < deadline) {
                pid_t w = waitpid(child_pid, &status, WNOHANG);
                if (w > 0) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (waitpid(child_pid, &status, WNOHANG) <= 0) {
                kill(child_pid, SIGKILL);
                waitpid(child_pid, &status, 0);
            }
            child_pid = -1;
        }
        if (stdin_fd >= 0) { ::close(stdin_fd); stdin_fd = -1; }
        if (stdout_fd >= 0) { ::close(stdout_fd); stdout_fd = -1; }
        if (stderr_drain.joinable()) { stderr_drain.join(); }
        if (stderr_fd >= 0) { ::close(stderr_fd); stderr_fd = -1; }
#endif
        connected = false;
    }

    bool write_message(const nlohmann::json& msg) {
        std::string body = msg.dump();
        std::string frame = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
#ifdef _WIN32
        DWORD total_written = 0;
        while (total_written < static_cast<DWORD>(frame.size())) {
            DWORD written = 0;
            BOOL ok = WriteFile(stdin_fd, frame.c_str() + total_written,
                               static_cast<DWORD>(frame.size() - total_written), &written, nullptr);
            if (!ok || written == 0) return false;
            total_written += written;
        }
        return true;
#else
        size_t total = 0;
        while (total < frame.size()) {
            ssize_t written = ::write(stdin_fd, frame.c_str() + total, frame.size() - total);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) return false;
            total += written;
        }
        return true;
#endif
    }

    nlohmann::json read_message(int timeout_ms = 30000) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

        auto check_timeout = [&]() -> bool {
            return std::chrono::steady_clock::now() < deadline;
        };

        auto wait_for_byte = [&](char& c) -> bool {
            while (check_timeout()) {
                if (!wait_for_data(100)) continue;
#ifdef _WIN32
                DWORD n = 0;
                if (ReadFile(stdout_fd, &c, 1, &n, nullptr) && n > 0) return true;
#else
                ssize_t n = ::read(stdout_fd, &c, 1);
                if (n < 0 && errno == EINTR) continue;
                if (n > 0) return true;
#endif
                return false;
            }
            return false;
        };

        std::string headers;
        char c;
        constexpr size_t MAX_HEADER_SIZE = 8192;
        while (true) {
            if (!wait_for_byte(c)) return nullptr;
            headers += c;
            if (headers.size() > MAX_HEADER_SIZE) return nullptr;
            if (headers.size() >= 4 &&
                headers.substr(headers.size() - 4) == "\r\n\r\n") {
                break;
            }
        }

        size_t content_length = 0;
        std::istringstream iss(headers);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.size() > 16 && line.substr(0, 16) == "Content-Length: ") {
                try {
                    content_length = std::stoul(line.substr(16));
                } catch (const std::exception&) {
                    return nullptr;
                }
            }
        }

        constexpr size_t MAX_BODY_SIZE = 100 * 1024 * 1024;
        if (content_length == 0 || content_length > MAX_BODY_SIZE) return nullptr;

        std::string body;
        body.resize(content_length);
        size_t total_read = 0;
        while (total_read < content_length) {
            if (!check_timeout()) return nullptr;
            if (!wait_for_data(100)) continue;
#ifdef _WIN32
            DWORD n = 0;
            if (!ReadFile(stdout_fd, &body[total_read],
                           static_cast<DWORD>(content_length - total_read), &n, nullptr) || n <= 0)
                return nullptr;
#else
            ssize_t n = ::read(stdout_fd, &body[total_read], content_length - total_read);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return nullptr;
#endif
            total_read += n;
        }

        try {
            return nlohmann::json::parse(body);
        } catch (...) {
            return nullptr;
        }
    }

    bool wait_for_data(int timeout_ms) {
#ifdef _WIN32
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            DWORD available = 0;
            if (PeekNamedPipe(stdout_fd, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
#else
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(stdout_fd, &fds);

        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int ret = select(stdout_fd + 1, &fds, nullptr, nullptr, &tv);
        if (ret < 0 && errno == EINTR) return wait_for_data(timeout_ms);
        return ret > 0 && FD_ISSET(stdout_fd, &fds);
#endif
    }
};

McpClient::McpClient() : impl_(std::make_unique<Impl>()) {}
McpClient::~McpClient() { shutdown(); }

Result<std::unique_ptr<McpClient>>
McpClient::spawn(const std::string& cmd,
                  const std::vector<std::string>& args,
                  const std::unordered_map<std::string, std::string>& envs) {

    auto client = std::unique_ptr<McpClient>(new McpClient());
    client->impl_->cmd = cmd;
    client->impl_->args = args;
    client->impl_->envs = envs;

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };

    HANDLE hStdinRead, hStdinWrite;
    HANDLE hStdoutRead, hStdoutWrite;
    HANDLE hStderrRead, hStderrWrite;

    CreatePipe(&hStdinRead, &hStdinWrite, &sa, 0);
    CreatePipe(&hStdoutRead, &hStdoutWrite, &sa, 0);
    CreatePipe(&hStderrRead, &hStderrWrite, &sa, 0);

    SetHandleInformation(hStdinWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hStdoutRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hStderrRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = { sizeof(STARTUPINFOA) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hStdinRead;
    si.hStdOutput = hStdoutWrite;
    si.hStdError = hStderrWrite;

    std::string cmdline = cmd;
    for (const auto& arg : args) {
        cmdline += " \"" + arg + "\"";
    }

    std::string env_block;
    for (const auto& [key, value] : envs) {
        env_block += key + "=" + value;
        env_block += '\0';
    }
    if (!envs.empty()) {
        env_block += '\0';
    }

    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessA(
        nullptr,
        const_cast<char*>(cmdline.c_str()),
        nullptr, nullptr, TRUE,
        0,
        env_block.empty() ? nullptr : const_cast<char*>(env_block.c_str()),
        nullptr, &si, &pi);

    CloseHandle(hStdinRead);
    CloseHandle(hStdoutWrite);
    CloseHandle(hStderrWrite);

    if (!ok) {
        CloseHandle(hStdinWrite);
        CloseHandle(hStdoutRead);
        CloseHandle(hStderrRead);
        return Result<std::unique_ptr<McpClient>>::err(
            make_error(ErrorCode::McpError, "创建进程失败: " + cmd));
    }

    client->impl_->stdin_fd = hStdinWrite;
    client->impl_->stdout_fd = hStdoutRead;
    client->impl_->stderr_fd = hStderrRead;
    client->impl_->child_process = pi.hProcess;
    client->impl_->child_thread = pi.hThread;
    client->impl_->connected = true;

    spdlog::info("Spawned MCP client: {} (pid {})", cmd, pi.dwProcessId);
#else
    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
        if (stdin_pipe[0] >= 0) close(stdin_pipe[0]);
        if (stdin_pipe[1] >= 0) close(stdin_pipe[1]);
        if (stdout_pipe[0] >= 0) close(stdout_pipe[0]);
        if (stdout_pipe[1] >= 0) close(stdout_pipe[1]);
        if (stderr_pipe[0] >= 0) close(stderr_pipe[0]);
        if (stderr_pipe[1] >= 0) close(stderr_pipe[1]);
        return Result<std::unique_ptr<McpClient>>::err(
            make_error(ErrorCode::McpError, "创建管道失败"));
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        return Result<std::unique_ptr<McpClient>>::err(
            make_error(ErrorCode::McpError, std::string("fork 失败: ") + strerror(errno)));
    }
    if (pid == 0) {
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);

        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);

        close(stdin_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        setpgid(0, 0);

        for (const auto& [key, value] : envs) {
            setenv(key.c_str(), value.c_str(), 1);
        }

        std::vector<const char*> exec_args;
        exec_args.push_back(cmd.c_str());
        for (const auto& arg : args) {
            exec_args.push_back(arg.c_str());
        }
        exec_args.push_back(nullptr);

        execvp(cmd.c_str(), const_cast<char* const*>(exec_args.data()));
        _exit(127);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    client->impl_->stdin_fd = stdin_pipe[1];
    client->impl_->stdout_fd = stdout_pipe[0];
    client->impl_->stderr_fd = stderr_pipe[0];
    client->impl_->child_pid = pid;
    client->impl_->connected = true;

    int stderr_fd = stderr_pipe[0];
    client->impl_->stderr_drain = std::thread([stderr_fd]() {
        char buf[4096];
        ssize_t n;
        while ((n = ::read(stderr_fd, buf, sizeof(buf))) > 0 ||
               (n < 0 && errno == EINTR)) {
            // drain stderr to prevent pipe buffer full / zombie
        }
    });

    spdlog::info("Spawned MCP client: {} (pid {})", cmd, pid);
#endif

    return Result<std::unique_ptr<McpClient>>::ok(std::move(client));
}

Result<void> McpClient::initialize() {
    nlohmann::json params;
    params["protocolVersion"] = "2025-03-26";
    params["capabilities"] = {
        {"roots", {{"listChanged", true}}}
    };
    params["clientInfo"] = {
        {"name", "kacli"},
        {"version", "0.1.0"}
    };

    auto result = send_request("initialize", params);
    if (!result) {
        return Result<void>::err(result.error());
    }

    send_request("notifications/initialized");

    spdlog::info("MCP client initialized");
    return Result<void>::ok();
}

Result<std::vector<mcp::ToolInfo>> McpClient::list_tools() {
    auto result = send_request("tools/list");
    if (!result) {
        return Result<std::vector<mcp::ToolInfo>>::err(result.error());
    }

    std::vector<mcp::ToolInfo> tools;
    if (result->contains("tools") && (*result)["tools"].is_array()) {
        for (const auto& t : (*result)["tools"]) {
            mcp::ToolInfo info;
            info.name = t.value("name", "");
            info.description = t.value("description", "");
            if (t.contains("inputSchema")) {
                info.input_schema = t["inputSchema"];
            }
            tools.push_back(std::move(info));
        }
    }

    return Result<std::vector<mcp::ToolInfo>>::ok(std::move(tools));
}

Result<nlohmann::json> McpClient::call_tool(
    const std::string& name, const nlohmann::json& arguments) {

    nlohmann::json params;
    params["name"] = name;
    params["arguments"] = arguments;

    auto result = send_request("tools/call", params);
    if (!result) {
        return Result<nlohmann::json>::err(result.error());
    }

    return Result<nlohmann::json>::ok(*result);
}

void McpClient::shutdown() {
    impl_->shutdown();
}

Result<nlohmann::json> McpClient::send_request(
    const std::string& method, const nlohmann::json& params) {

    if (!impl_->connected) {
        return Result<nlohmann::json>::err(
            make_error(ErrorCode::McpError, "未连接"));
    }

    int id = ++impl_->request_id;

    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = id;
    request["method"] = method;
    if (!params.is_null() && !params.empty()) {
        request["params"] = params;
    }

    if (!impl_->write_message(request)) {
        return Result<nlohmann::json>::err(
            make_error(ErrorCode::McpError, "写入请求失败"));
    }

    if (method.find("notifications/") == 0) {
        return Result<nlohmann::json>::ok(nlohmann::json::object());
    }

    auto start = std::chrono::steady_clock::now();
    auto timeout = std::chrono::seconds(30);

    while (true) {
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > timeout) {
            return Result<nlohmann::json>::err(
                make_error(ErrorCode::McpError, "请求超时: " + method));
        }

        if (impl_->wait_for_data(100)) {
            auto response = impl_->read_message();
            if (response.is_null()) {
                return Result<nlohmann::json>::err(
                    make_error(ErrorCode::McpError, "读取响应失败"));
            }

            if (response.contains("id") && response["id"] == id) {
                if (response.contains("error")) {
                    std::string error_msg = response["error"].value("message", "unknown error");
                    return Result<nlohmann::json>::err(
                        make_error(ErrorCode::McpError, error_msg));
                }
                return Result<nlohmann::json>::ok(
                    response.value("result", nlohmann::json::object()));
            }
        }
    }
}

} // namespace goose
