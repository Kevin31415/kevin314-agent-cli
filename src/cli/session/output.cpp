#include "output.h"
#include "input.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <regex>
#include <spdlog/spdlog.h>

#ifdef _WIN32
    #include <windows.h>
    #include <io.h>
    static bool enable_ansi() {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD mode = 0;
        if (!GetConsoleMode(h, &mode)) return false;
        mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        return SetConsoleMode(h, mode) != 0;
    }
    static const bool ansi_enabled = enable_ansi();
#else
    #include <unistd.h>
#endif

namespace goose {
namespace cli {

static const char* RESET = "\033[0m";

// TUI 模式下由 set_render_stream 注入（聊天区），默认 stdout。
static std::ostream* g_render_stream = nullptr;
static std::ostream& render_out() { return g_render_stream ? *g_render_stream : std::cout; }

void set_render_stream(std::ostream* os) { g_render_stream = os; }

static std::string strip_ansi(const std::string& input) {
    static const std::regex ansi_regex(
        "\033\\[[0-9;]*[A-Za-z]"
        "|\033\\].*?(?:\033\\\\|\007)"
        "|\033\\[\\?\\d+[hl]"
        "|\033[\\[\\]()#][A-Za-z0-9]"
    );
    return std::regex_replace(input, ansi_regex, "");
}
static const char* BOLD = "\033[1m";
static const char* DIM = "\033[2m";
static const char* ITALIC = "\033[3m";
static const char* UNDERLINE = "\033[4m";
static const char* RED = "\033[31m";
static const char* GREEN = "\033[32m";
static const char* BLUE = "\033[34m";
static const char* MAGENTA = "\033[35m";
static const char* CYAN = "\033[36m";
static const char* GREY = "\033[90m";

std::string truncate_text(const std::string& text, size_t max_lines) {
    std::istringstream iss(text);
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(iss, line)) {
        lines.push_back(line);
    }

    if (lines.size() <= max_lines) {
        return text;
    }

    size_t head = max_lines / 2;
    size_t tail = max_lines - head;

    std::ostringstream oss;
    for (size_t i = 0; i < head; i++) {
        oss << lines[i] << "\n";
    }
    oss << DIM << "... (" << (lines.size() - max_lines) << " 行已隐藏) ..." << RESET << "\n";
    for (size_t i = lines.size() - tail; i < lines.size(); i++) {
        oss << lines[i] << "\n";
    }
    return oss.str();
}

// 行内标记渲染：**加粗**（内部可嵌套 `code`、*斜体*、[链接]）、*斜体*、
// `行内代码`、[链接](url)。递归扫描 [begin, end)，找不到闭合标记时按原样输出，
// 因此未闭合/错误嵌套不会破坏整个行。
static void render_inline_scan(const std::string& s, size_t begin, size_t end,
                               std::ostringstream& out) {
    size_t i = begin;
    while (i < end) {
        char c = s[i];
        if (c == '`') {
            size_t close = s.find('`', i + 1);
            if (close != std::string::npos && close < end) {
                out << CYAN << s.substr(i + 1, close - i - 1) << RESET;
                i = close + 1;
                continue;
            }
            out << c;
            i++;
            continue;
        }
        if (c == '*' && i + 1 < end && s[i + 1] == '*') {
            size_t close = s.find("**", i + 2);
            if (close != std::string::npos && close < end) {
                out << BOLD;
                render_inline_scan(s, i + 2, close, out);
                out << RESET;
                i = close + 2;
                continue;
            }
            out << c;
            i++;
            continue;
        }
        if (c == '*') {
            size_t close = s.find('*', i + 1);
            if (close != std::string::npos && close < end) {
                out << ITALIC;
                render_inline_scan(s, i + 1, close, out);
                out << RESET;
                i = close + 1;
                continue;
            }
            out << c;
            i++;
            continue;
        }
        if (c == '[') {
            size_t close_bracket = s.find(']', i + 1);
            if (close_bracket != std::string::npos && close_bracket < end &&
                close_bracket + 1 < end && s[close_bracket + 1] == '(') {
                size_t close_paren = s.find(')', close_bracket + 2);
                if (close_paren != std::string::npos && close_paren < end) {
                    out << UNDERLINE << BLUE;
                    render_inline_scan(s, i + 1, close_bracket, out);
                    out << RESET;
                    i = close_paren + 1;
                    continue;
                }
            }
            out << c;
            i++;
            continue;
        }
        out << c;
        i++;
    }
}

static std::string render_inline_markdown(const std::string& line) {
    std::ostringstream out;
    render_inline_scan(line, 0, line.size(), out);
    return out.str();
}

// 渲染单行 markdown。代码块状态（是否在代码块内、语言）由调用方维护，
// 以便整块渲染与流式渲染共用同一套规则。
static void render_markdown_line(const std::string& line, bool& in_code_block,
                                 std::string& code_lang, std::ostringstream& out) {
    if (in_code_block) {
        if (line.rfind("```", 0) == 0) {
            in_code_block = false;
            code_lang.clear();
            out << DIM << "└─" << RESET << "\n";
        } else {
            out << DIM << "│ " << RESET << line << "\n";
        }
        return;
    }

    if (line.rfind("```", 0) == 0) {
        in_code_block = true;
        code_lang = line.substr(3);
        out << DIM << "┌─ " << code_lang << RESET << "\n";
        return;
    }

    if (!line.empty() && line[0] == '#') {
        size_t hashes = 0;
        while (hashes < line.size() && line[hashes] == '#') hashes++;
        std::string content = line.substr(hashes);
        size_t start = content.find_first_not_of(" ");
        if (start != std::string::npos) content = content.substr(start);
        if (hashes <= 2) {
            out << BOLD << CYAN << content << RESET << "\n";
        } else {
            out << BOLD << content << RESET << "\n";
        }
        return;
    }

    // 列表项：`- `、`* `、`+ `、`• `，允许行首缩进（嵌套列表）。内容仍走行内渲染。
    {
        size_t content_start = 0;
        while (content_start < line.size() &&
               (line[content_start] == ' ' || line[content_start] == '\t')) {
            content_start++;
        }
        if (content_start < line.size()) {
            char bullet = line[content_start];
            bool is_bullet = (bullet == '-' || bullet == '*' || bullet == '+') &&
                             content_start + 1 < line.size() && line[content_start + 1] == ' ';
            bool is_utf8_bullet = line.compare(content_start, 3, "•") == 0 &&
                                  content_start + 3 < line.size() && line[content_start + 3] == ' ';
            if (is_bullet || is_utf8_bullet) {
                out << GREEN << "  • " << RESET
                    << render_inline_markdown(line.substr(content_start + (is_utf8_bullet ? 4 : 2)))
                    << "\n";
                return;
            }
        }
    }

    if (line.size() >= 2 && line.rfind("> ", 0) == 0) {
        out << DIM << "│ " << RESET << ITALIC
            << render_inline_markdown(line.substr(2)) << RESET << "\n";
        return;
    }

    if (line.find_first_not_of("- ") == std::string::npos && line.find('-') != std::string::npos) {
        out << DIM << "────────────────────────────────────────" << RESET << "\n";
        return;
    }

    out << render_inline_markdown(line) << "\n";
}

std::string MarkdownStream::feed(const std::string& delta) {
    std::string clean = strip_ansi(delta);
    line_buffer_ += clean;

    std::ostringstream out;
    size_t pos;
    while ((pos = line_buffer_.find('\n')) != std::string::npos) {
        std::string line = line_buffer_.substr(0, pos);
        line_buffer_.erase(0, pos + 1);
        render_markdown_line(line, in_code_block_, code_lang_, out);
    }

    // 长行保护：无换行的超长增量直接透出，避免无限缓冲。
    if (line_buffer_.size() > kMaxBufferedChars) {
        out << line_buffer_;
        line_buffer_.clear();
    }
    return out.str();
}

std::string MarkdownStream::flush() {
    std::ostringstream out;
    if (!line_buffer_.empty()) {
        render_markdown_line(line_buffer_, in_code_block_, code_lang_, out);
        line_buffer_.clear();
    }
    if (in_code_block_) {
        out << DIM << "└─" << RESET << "\n";
        in_code_block_ = false;
        code_lang_.clear();
    }
    return out.str();
}

std::string render_markdown(const std::string& text) {
    std::string clean = strip_ansi(text);
    std::istringstream iss(clean);
    std::string line;
    std::ostringstream result;
    bool in_code_block = false;
    std::string code_lang;

    while (std::getline(iss, line)) {
        render_markdown_line(line, in_code_block, code_lang, result);
    }
    if (in_code_block) {
        result << DIM << "└─" << RESET << "\n";
    }

    return result.str();
}

void render_message(const Message& msg, bool include_thinking) {
    for (const auto& block : msg.content) {
        if (std::holds_alternative<TextContent>(block)) {
            const auto& text = std::get<TextContent>(block).text;
            if (!text.empty()) {
                render_out() << render_markdown(text);
            }
        } else if (std::holds_alternative<ThinkingContent>(block)) {
            if (!include_thinking) continue;
            const auto& thinking = std::get<ThinkingContent>(block).thinking;
            if (!thinking.empty()) {
                std::istringstream iss(strip_ansi(thinking));
                std::string line;
                while (std::getline(iss, line)) {
                    render_out() << GREY << "    " << line << RESET << "\n";
                }
            }
        } else if (std::holds_alternative<ToolRequest>(block)) {
            render_tool_request(std::get<ToolRequest>(block));
        } else if (std::holds_alternative<ToolResponse>(block)) {
            render_tool_response(std::get<ToolResponse>(block));
        }
    }
}

void render_tool_request(const ToolRequest& req) {
    try {
        if (std::holds_alternative<CallToolRequestParams>(req.tool_call)) {
            const auto& params = std::get<CallToolRequestParams>(req.tool_call);
            render_out() << MAGENTA << BOLD << "▸ " << strip_ansi(params.name) << RESET;

            if (!params.arguments.is_null() && !params.arguments.empty()) {
                if (params.arguments.contains("command") && params.arguments["command"].is_string()) {
                    render_out() << " " << DIM << strip_ansi(params.arguments["command"].get<std::string>()) << RESET;
                } else if (params.arguments.contains("path") && params.arguments["path"].is_string()) {
                    render_out() << " " << DIM << strip_ansi(params.arguments["path"].get<std::string>()) << RESET;
                }
            }
            render_out() << "\n";
        } else if (std::holds_alternative<std::string>(req.tool_call)) {
            render_out() << RED << "▸ [工具错误: " << strip_ansi(std::get<std::string>(req.tool_call)) << "]" << RESET << "\n";
        }
    } catch (const std::exception& e) {
        render_out() << RED << "▸ [渲染错误: " << e.what() << "]" << RESET << "\n";
    }
}

void render_tool_response(const ToolResponse& resp) {
    // 工具结果不再回显；仅在失败时标记错误。
    if (!std::holds_alternative<CallToolResult>(resp.tool_result)) {
        return;
    }
    const auto& result = std::get<CallToolResult>(resp.tool_result);
    if (result.is_error) {
        render_out() << RED << "  ✗ 工具执行失败" << RESET << "\n";
    }
}

void render_error(const std::string& error) {
    render_out() << RED << "错误: " << strip_ansi(error) << RESET << "\n";
}

void render_agent_event(const AgentEvent& event) {
    static MarkdownStream markdown_stream;
    static bool reasoning_active = false;
    static bool streamed_reasoning = false;
    static int reasoning_pending_newlines = 0;
    switch (event.type) {
        case AgentEventType::TextDelta:
            if (reasoning_active) {
                render_out() << RESET << "\n";
                reasoning_active = false;
                reasoning_pending_newlines = 0;
            }
            render_out() << markdown_stream.feed(event.text_delta) << std::flush;
            break;
        case AgentEventType::ReasoningDelta: {
            std::string clean = strip_ansi(event.reasoning_delta);
            if (!reasoning_active) {
                render_out() << "\n" << GREY << "    ";
                reasoning_active = true;
            }
            streamed_reasoning = true;
            for (char c : clean) {
                if (c == '\n') {
                    render_out() << c;
                    reasoning_pending_newlines++;
                } else {
                    // 首段缩进一次；自然段（空行分隔）重新缩进；
                    // 段内折行不缩进，只靠灰色延续。
                    if (reasoning_pending_newlines >= 2) {
                        render_out() << GREY << "    ";
                    }
                    reasoning_pending_newlines = 0;
                    render_out() << c;
                }
            }
            break;
        }
        case AgentEventType::Message:
            if (reasoning_active) {
                render_out() << RESET << "\n";
                reasoning_active = false;
                reasoning_pending_newlines = 0;
            }
            if (event.msg) {
                bool has_text = false;
                for (const auto& block : event.msg->content) {
                    if (std::holds_alternative<TextContent>(block)) {
                        has_text = true;
                        break;
                    }
                }
                if (!has_text) {
                    // 每条流结束（含工具调用轮次）都冲刷未完成的行与代码块。
                    render_out() << markdown_stream.flush();
                    render_message(*event.msg, !streamed_reasoning);
                } else {
                    render_out() << markdown_stream.flush() << "\n";
                }
                streamed_reasoning = false;
            }
            break;
        case AgentEventType::Usage:
            if (event.provider_usage) {
                const auto& usage = event.provider_usage->usage;
                spdlog::debug("Tokens - input: {}, output: {}",
                    usage.input_tokens.value_or(0),
                    usage.output_tokens.value_or(0));
            }
            break;
    }
}

}} // namespace goose::cli
