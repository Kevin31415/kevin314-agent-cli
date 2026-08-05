#include "developer_extension.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstdlib>
#include <array>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <chrono>

#ifdef _WIN32
    #include <windows.h>
    #include <io.h>
    #include <process.h>
#else
    #include <sys/select.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

namespace fs = std::filesystem;

static const size_t kMaxOutput = 50 * 1024;

static std::string resolve_path(const std::string& path, const std::string& cwd) {
    if (fs::path(path).is_absolute()) return path;
    return (fs::path(cwd) / path).string();
}

namespace goose {

DeveloperExtension::DeveloperExtension() {
    working_dir_ = fs::current_path().string();
}

std::vector<Tool> DeveloperExtension::list_tools() const {
    std::vector<Tool> tools;

    {
        Tool t;
        t.name = "write";
        t.description = "Create or overwrite a file with the given content";
        t.read_only_hint = false;
        t.input_schema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"}, {"description", "File path"}}},
                {"content", {{"type", "string"}, {"description", "File content"}}}
            }},
            {"required", nlohmann::json::array({"path", "content"})}
        };
        tools.push_back(std::move(t));
    }

    {
        Tool t;
        t.name = "edit";
        t.description = "Find and replace text in a file";
        t.read_only_hint = false;
        t.input_schema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"}, {"description", "File path"}}},
                {"before", {{"type", "string"}, {"description", "Text to find"}}},
                {"after", {{"type", "string"}, {"description", "Replacement text"}}}
            }},
            {"required", nlohmann::json::array({"path", "before", "after"})}
        };
        tools.push_back(std::move(t));
    }

    {
        Tool t;
        t.name = "shell";
        t.description = "Execute a shell command";
        t.read_only_hint = false;
        t.input_schema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"command", {{"type", "string"}, {"description", "Shell command"}}},
                {"timeout_secs", {{"type", "integer"}, {"description", "Timeout in seconds"}, {"default", 120}}}
            }},
            {"required", nlohmann::json::array({"command"})}
        };
        tools.push_back(std::move(t));
    }

    {
        Tool t;
        t.name = "tree";
        t.description = "Display directory tree";
        t.read_only_hint = true;
        t.input_schema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"}, {"description", "Directory path"}, {"default", "."}}},
                {"depth", {{"type", "integer"}, {"description", "Max depth"}, {"default", 2}}}
            }},
            {"required", nlohmann::json::array()}
        };
        tools.push_back(std::move(t));
    }

    {
        Tool t;
        t.name = "read";
        t.description = "Read file contents";
        t.read_only_hint = true;
        t.input_schema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"}, {"description", "File path"}}}
            }},
            {"required", nlohmann::json::array({"path"})}
        };
        tools.push_back(std::move(t));
    }

    return tools;
}

Result<nlohmann::json> DeveloperExtension::call_tool(
    const std::string& name,
    const nlohmann::json& arguments,
    const std::string& working_dir) {

    std::string cwd = working_dir.empty() ? working_dir_ : working_dir;

    if (name == "write") return handle_write(arguments, cwd);
    if (name == "edit") return handle_edit(arguments, cwd);
    if (name == "shell") return handle_shell(arguments, cwd);
    if (name == "tree") return handle_tree(arguments, cwd);
    if (name == "read") return handle_read(arguments, cwd);

    return Result<nlohmann::json>::err(
        make_error(ErrorCode::ExtensionError, "未知工具: " + name));
}

Result<nlohmann::json> DeveloperExtension::handle_write(
    const nlohmann::json& args, const std::string& cwd) {

    std::string path = args.at("path").get<std::string>();
    std::string content = args.at("content").get<std::string>();

    path = resolve_path(path, cwd);

    try {
        fs::create_directories(fs::path(path).parent_path());

        bool existed = fs::exists(path);
        std::ofstream ofs(path);
        if (!ofs.is_open()) {
            return Result<nlohmann::json>::err(
                make_error(ErrorCode::IoError, "无法打开文件写入: " + path));
        }
        ofs << content;
        ofs.close();

        int line_count = 1;
        for (char c : content) {
            if (c == '\n') line_count++;
        }
        if (!content.empty() && content.back() == '\n') line_count--;

        std::string prefix = existed ? "Wrote" : "Created";
        return Result<nlohmann::json>::ok(
            nlohmann::json{{"output", prefix + " " + path + " (" + std::to_string(line_count) + " lines)"}});
    } catch (const std::exception& e) {
        return Result<nlohmann::json>::err(
            make_error(ErrorCode::IoError, std::string("写入失败: ") + e.what()));
    }
}

Result<nlohmann::json> DeveloperExtension::handle_edit(
    const nlohmann::json& args, const std::string& cwd) {

    std::string path = args.at("path").get<std::string>();
    std::string before = args.at("before").get<std::string>();
    std::string after = args.at("after").get<std::string>();

    if (before.empty()) {
        return Result<nlohmann::json>::err(
            make_error(ErrorCode::ExtensionError, "before 不能为空"));
    }

    path = resolve_path(path, cwd);

    try {
        std::ifstream ifs(path);
        if (!ifs.is_open()) {
            return Result<nlohmann::json>::err(
                make_error(ErrorCode::IoError, "文件未找到: " + path));
        }
        std::string content((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());
        ifs.close();

        size_t count = 0;
        size_t pos = 0;
        while ((pos = content.find(before, pos)) != std::string::npos) {
            count++;
            pos += before.length();
        }

        if (count == 0) {
            size_t preview_len = std::min(content.length(), (size_t)500);
            return Result<nlohmann::json>::err(
                make_error(ErrorCode::ExtensionError,
                    "Text not found in " + path + "\nPreview:\n" + content.substr(0, preview_len)));
        }
        if (count > 1) {
            return Result<nlohmann::json>::err(
                make_error(ErrorCode::ExtensionError,
                    "Text appears " + std::to_string(count) + " times in " + path + "; must be unique"));
        }

        pos = content.find(before);
        content.replace(pos, before.length(), after);

        std::ofstream ofs(path);
        if (!ofs.is_open()) {
            return Result<nlohmann::json>::err(
                make_error(ErrorCode::IoError, "无法打开文件写入: " + path));
        }
        ofs << content;
        ofs.close();

        return Result<nlohmann::json>::ok(
            nlohmann::json{{"output", "Edited " + path}});
    } catch (const std::exception& e) {
        return Result<nlohmann::json>::err(
            make_error(ErrorCode::IoError, std::string("编辑失败: ") + e.what()));
    }
}

Result<nlohmann::json> DeveloperExtension::handle_shell(
    const nlohmann::json& args, const std::string& cwd) {

    try {
        std::string command = args.at("command").get<std::string>();
        uint64_t timeout_secs = args.value("timeout_secs", 120u);
        if (timeout_secs > 86400) timeout_secs = 86400;

        auto result = run_shell(command, timeout_secs, cwd);

        nlohmann::json output;
        output["stdout"] = result.stdout_output;
        output["stderr"] = result.stderr_output;
        output["exit_code"] = result.exit_code;
        output["timed_out"] = result.timed_out;

        return Result<nlohmann::json>::ok(std::move(output));
    } catch (const std::exception& e) {
        return Result<nlohmann::json>::err(
            make_error(ErrorCode::ExtensionError, std::string("执行命令失败: ") + e.what()));
    }
}

Result<nlohmann::json> DeveloperExtension::handle_tree(
    const nlohmann::json& args, const std::string& cwd) {

    std::string path = args.value("path", ".");
    int depth = args.value("depth", 2);
    if (depth < 0) depth = 0;

    path = resolve_path(path, cwd);

    if (!fs::exists(path)) {
        return Result<nlohmann::json>::err(
            make_error(ErrorCode::IoError, "路径未找到: " + path));
    }

    std::ostringstream oss;
    auto max_depth = static_cast<unsigned int>(depth);

    try {
    constexpr size_t kMaxTreeEntries = 2000;
    bool truncated = false;
    size_t emitted = 0;

    std::function<void(const fs::path&, int)> walk = [&](const fs::path& dir, int current_depth) {
        if (static_cast<unsigned int>(current_depth) > max_depth) return;
        if (truncated) return;

        std::vector<fs::directory_entry> entries;
        for (const auto& entry : fs::directory_iterator(dir)) {
            entries.push_back(entry);
        }
        std::sort(entries.begin(), entries.end());

        for (const auto& entry : entries) {
            if (truncated) return;

            std::string name = entry.path().filename().string();
            if (name.starts_with(".")) continue;

            if (emitted >= kMaxTreeEntries) {
                truncated = true;
                return;
            }

            for (int i = 0; i < current_depth; i++) oss << "  ";

            if (entry.is_directory()) {
                oss << name << "/\n";
                emitted++;
                walk(entry.path(), current_depth + 1);
            } else {
                try {
                auto size = entry.file_size();
                oss << name;
                if (size > 1024 * 1024) oss << " (" << (size / (1024 * 1024)) << "MB)";
                else if (size > 1024) oss << " (" << (size / 1024) << "KB)";
                oss << "\n";
                emitted++;
                } catch (...) {
                    oss << name << " (unknown size)\n";
                    emitted++;
                }
            }
        }
    };

    walk(path, 0);

    if (truncated) {
        oss << "...(truncated: 目录条目超过 " << kMaxTreeEntries << " 个)...\n";
    }
    } catch (const std::exception& e) {
        return Result<nlohmann::json>::err(
            make_error(ErrorCode::IoError, std::string("遍历目录失败: ") + e.what()));
    }

    return Result<nlohmann::json>::ok(
        nlohmann::json{{"output", oss.str()}});
}

Result<nlohmann::json> DeveloperExtension::handle_read(
    const nlohmann::json& args, const std::string& cwd) {

    std::string path = args.at("path").get<std::string>();

    path = resolve_path(path, cwd);

    constexpr size_t kMaxFileSize = 1024 * 1024;

    try {
        std::ifstream ifs(path);
        if (!ifs.is_open()) {
            return Result<nlohmann::json>::err(
                make_error(ErrorCode::IoError, "文件不存在或无法打开: " + path));
        }

        std::error_code ec;
        uintmax_t file_size = std::filesystem::file_size(path, ec);
        bool truncated = false;
        if (!ec && file_size > kMaxFileSize) {
            ifs.seekg(static_cast<std::ifstream::pos_type>(file_size - kMaxFileSize));
            truncated = true;
        }

        std::string content((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());

        if (truncated) {
            content = "...(truncated)...\n" + content;
        }

        return Result<nlohmann::json>::ok(
            nlohmann::json{{"output", content}});
    } catch (const std::exception& e) {
        return Result<nlohmann::json>::err(
            make_error(ErrorCode::IoError, std::string("读取失败: ") + e.what()));
    }
}

ShellResult DeveloperExtension::run_shell(
    const std::string& command, uint64_t timeout_secs, const std::string& working_dir) {

    ShellResult result;
    bool stdout_truncated = false;
    bool stderr_truncated = false;

    // 输出执行期间就按尾部上限截断，避免嘈杂命令无界累积内存。
    auto append_capped = [](std::string& output, const char* buf, size_t n, bool& truncated) {
        if (output.size() + n > kMaxOutput) {
            output.erase(0, output.size() + n - kMaxOutput);
            truncated = true;
        }
        output.append(buf, n);
    };

#ifdef _WIN32
    std::string shell = "cmd.exe";
    const char* goose_shell = std::getenv("GOOSE_SHELL");
    if (!goose_shell) goose_shell = std::getenv("KACLI_SHELL");
    if (goose_shell) {
        shell = goose_shell;
    }

    HANDLE hStdoutRead, hStdoutWrite;
    HANDLE hStderrRead, hStderrWrite;
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };

    CreatePipe(&hStdoutRead, &hStdoutWrite, &sa, 0);
    CreatePipe(&hStderrRead, &hStderrWrite, &sa, 0);
    SetHandleInformation(hStdoutWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hStderrWrite, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = { sizeof(STARTUPINFOA) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hStdoutWrite;
    si.hStdError = hStderrWrite;

    // Double embedded quotes so a malicious command cannot break out of the
    // cmd.exe /C "..." wrapper and inject additional commands.
    std::string escaped_command;
    escaped_command.reserve(command.size());
    for (char c : command) {
        if (c == '"') escaped_command += "\"\"";
        else escaped_command += c;
    }

    std::string cmdline = shell + " /S /C \"" + escaped_command + "\"";

    PROCESS_INFORMATION pi = {};
    std::string work_dir_str = working_dir;

    BOOL ok = CreateProcessA(
        nullptr,
        const_cast<char*>(cmdline.c_str()),
        nullptr, nullptr, TRUE, 0,
        nullptr,
        work_dir_str.empty() ? nullptr : work_dir_str.c_str(),
        &si, &pi);

    CloseHandle(hStdoutWrite);
    CloseHandle(hStderrWrite);

    if (!ok) {
        CloseHandle(hStdoutRead);
        CloseHandle(hStderrRead);
        result.exit_code = -1;
        result.stderr_output = "Failed to create process";
        return result;
    }

    auto peek_available = [](HANDLE h) -> DWORD {
        DWORD avail = 0;
        if (PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) return avail;
        return 0;
    };
    auto read_available = [&append_capped](HANDLE h, std::string& output, bool& truncated) {
        char buf[4096];
        DWORD n = 0;
        if (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) {
            append_capped(output, buf, static_cast<size_t>(n), truncated);
        }
    };

    DWORD timeout_ms = (timeout_secs > 4294967) ? INFINITE : static_cast<DWORD>(timeout_secs * 1000);
    DWORD wait_result = WaitForSingleObject(pi.hProcess, timeout_ms);

    // 进程退出后仍有孙进程持有管道写端时不会 EOF；用有界轮询排空，
    // 避免 join 阻塞读线程导致永久死锁。
    auto drain_remaining = [&]() {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < deadline) {
            DWORD avail_out = peek_available(hStdoutRead);
            DWORD avail_err = peek_available(hStderrRead);
            if (avail_out == 0 && avail_err == 0) break;
            if (avail_out > 0) read_available(hStdoutRead, result.stdout_output, stdout_truncated);
            if (avail_err > 0) read_available(hStderrRead, result.stderr_output, stderr_truncated);
        }
    };
    drain_remaining();

    CloseHandle(hStdoutRead);
    CloseHandle(hStderrRead);

    if (wait_result == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 1000);
        result.timed_out = true;
        result.exit_code = -1;
    } else {
        DWORD exit_code = 0;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        result.exit_code = static_cast<int>(exit_code);
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
#else
    std::string shell = "sh";
    const char* goose_shell = std::getenv("GOOSE_SHELL");
    if (!goose_shell) goose_shell = std::getenv("KACLI_SHELL");
    if (goose_shell) {
        shell = goose_shell;
    } else if (system("which bash >/dev/null 2>&1") == 0) {
        shell = "bash";
    }

    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    if (pipe(stdout_pipe) != 0) {
        result.exit_code = -1;
        result.stderr_output = "Failed to create stdout pipe";
        return result;
    }
    if (pipe(stderr_pipe) != 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        result.exit_code = -1;
        result.stderr_output = "Failed to create stderr pipe";
        return result;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        result.exit_code = -1;
        result.stderr_output = std::string("fork failed: ") + strerror(errno);
        return result;
    }
    if (pid == 0) {
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        if (!working_dir.empty()) {
            if (chdir(working_dir.c_str()) != 0) _exit(127);
        }

        setpgid(0, 0);
        execlp(shell.c_str(), shell.c_str(), "-c", command.c_str(), nullptr);
        _exit(127);
    }

    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    auto start_time = std::chrono::steady_clock::now();
    bool timed_out = false;
    const int max_fd = std::max(stdout_pipe[0], stderr_pipe[0]);

    auto read_available = [&append_capped](int fd, std::string& output, bool& truncated) -> bool {
        char buf[4096];
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            append_capped(output, buf, static_cast<size_t>(n), truncated);
            return true;
        }
        return false;
    };

    // 有界排空：进程退出后孙进程可能仍持有管道写端（后台任务），不会出现
    // EOF；在截止时间内读走可用数据，避免阻塞读导致永久挂死。
    auto drain_remaining = [&]() {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < deadline) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(stdout_pipe[0], &fds);
            FD_SET(stderr_pipe[0], &fds);
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 100000;
            int ret = select(max_fd + 1, &fds, nullptr, nullptr, &tv);
            if (ret <= 0) break;
            if (FD_ISSET(stdout_pipe[0], &fds)) {
                read_available(stdout_pipe[0], result.stdout_output, stdout_truncated);
            }
            if (FD_ISSET(stderr_pipe[0], &fds)) {
                read_available(stderr_pipe[0], result.stderr_output, stderr_truncated);
            }
        }
    };

    while (true) {
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        uint64_t timeout_ms_val = (timeout_secs > UINT64_MAX / 1000)
            ? UINT64_MAX
            : static_cast<uint64_t>(timeout_secs) * 1000;
        if (static_cast<uint64_t>(elapsed_ms) >= timeout_ms_val) {
            timed_out = true;
            break;
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(stdout_pipe[0], &fds);
        FD_SET(stderr_pipe[0], &fds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000;

        int ret = select(max_fd + 1, &fds, nullptr, nullptr, &tv);
        if (ret > 0) {
            if (FD_ISSET(stdout_pipe[0], &fds)) {
                read_available(stdout_pipe[0], result.stdout_output, stdout_truncated);
            }
            if (FD_ISSET(stderr_pipe[0], &fds)) {
                read_available(stderr_pipe[0], result.stderr_output, stderr_truncated);
            }
        }

        int status;
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w > 0) {
            drain_remaining();
            if (WIFEXITED(status)) {
                result.exit_code = WEXITSTATUS(status);
            } else {
                result.exit_code = -1;
            }
            break;
        }
    }

    close(stdout_pipe[0]);
    close(stderr_pipe[0]);

    if (timed_out) {
        kill(-pid, SIGKILL);
        int status;
        waitpid(pid, &status, 0);
        result.timed_out = true;
        result.exit_code = -1;
    }
#endif

    if (stdout_truncated) {
        result.stdout_output = "...(truncated)...\n" + result.stdout_output;
    }
    if (stderr_truncated) {
        result.stderr_output = "...(truncated)...\n" + result.stderr_output;
    }

    return result;
}

} // namespace goose
