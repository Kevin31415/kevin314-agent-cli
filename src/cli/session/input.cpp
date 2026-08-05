#include "input.h"
#include <iostream>
#include <sstream>
#include <algorithm>

#ifdef _WIN32
    #include <conio.h>
#else
    #include <unistd.h>
    #include <termios.h>
    #include <signal.h>
#endif

namespace goose {
namespace cli {

#ifndef _WIN32
static struct termios g_saved_termios;
static void restore_termios_and_raise(int sig) {
    tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios);
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

static std::vector<std::string> split_args(const std::string& s) {
    std::vector<std::string> args;
    std::istringstream iss(s);
    std::string token;
    while (iss >> token) {
        if (!token.empty() && token[0] == '"') {
            std::string full = token.substr(1);
            bool closed = false;
            while (iss >> token) {
                if (!token.empty() && token.back() == '"') {
                    full += " " + token.substr(0, token.size() - 1);
                    closed = true;
                    break;
                }
                full += " " + token;
            }
            if (!closed && !full.empty()) {
                full = "\"" + full;
            }
            args.push_back(full);
        } else {
            args.push_back(token);
        }
    }
    return args;
}

ParsedInput parse_input(const std::string& raw) {
    ParsedInput result;
    result.text = raw;

    std::string trimmed = raw;
    trimmed.erase(0, trimmed.find_first_not_of(" \t"));
    trimmed.erase(trimmed.find_last_not_of(" \t") + 1);

    if (trimmed.empty()) {
        result.type = InputResult::Retry;
        return result;
    }

    if (trimmed == "exit" || trimmed == "quit" || trimmed == "/exit" || trimmed == "/quit") {
        result.type = InputResult::Exit;
        result.command = "exit";
        return result;
    }

    if (trimmed[0] == '/') {
        result.type = InputResult::SlashCommand;
        size_t space = trimmed.find(' ');
        if (space != std::string::npos) {
            result.command = trimmed.substr(0, space);
            auto args = split_args(trimmed.substr(space + 1));
            result.args = std::move(args);
        } else {
            result.command = trimmed;
        }
        return result;
    }

    result.type = InputResult::Message;
    return result;
}

#ifndef _WIN32
static std::string read_line_raw(const std::string& prompt) {
    struct termios oldt;
    if (tcgetattr(STDIN_FILENO, &oldt) != 0) {
        std::string input;
        std::getline(std::cin, input);
        return input;
    }
    g_saved_termios = oldt;

    struct sigaction sa{};
    sa.sa_handler = restore_termios_and_raise;
    struct sigaction old_int, old_term;
    sigaction(SIGINT, &sa, &old_int);
    sigaction(SIGTERM, &sa, &old_term);

    struct termios newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    // prompt 可以含开头的换行（如 "\n> "）；换行只在首次显示时输出，
    // 重绘只用换行之后的行内提示符，避免每次输入/退格都产生新行。
    std::string prompt_tail = prompt;
    auto nl = prompt.find_last_of('\n');
    if (nl != std::string::npos) prompt_tail = prompt.substr(nl + 1);

    std::string buf;
    bool eof = false;
    while (true) {
        char c = 0;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) { eof = true; break; }  // EOF / terminal hangup → 退出
        if (n != 1) break;

        if (c == '\n' || c == '\r') {
            break;
        }
        if (c == 127 || c == 8) {  // DEL / Backspace
            if (buf.empty()) continue;
            size_t i = buf.size() - 1;
            while (i > 0 && (buf[i] & 0xC0) == 0x80) i--;
            buf.resize(i);
        } else {
            buf += c;
        }
        // 整行重绘：避免多字节（中文）退格按字节擦除留下的残留。
        std::cout << "\r" << prompt_tail << buf << "\x1b[K" << std::flush;
    }

    std::cout << "\n";

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    sigaction(SIGINT, &old_int, nullptr);
    sigaction(SIGTERM, &old_term, nullptr);

    return eof ? "exit" : buf;
}
#endif

std::string read_line(const std::string& prompt) {
    std::cout << prompt << std::flush;
#ifndef _WIN32
    if (isatty(STDIN_FILENO)) {
        return read_line_raw(prompt);
    }
#endif
    std::string input;
    if (!std::getline(std::cin, input)) {
        return "exit";
    }
    return input;
}

std::optional<char> read_key() {
#ifdef _WIN32
    if (_kbhit()) return static_cast<char>(_getch());
    return std::nullopt;
#else
    if (!isatty(STDIN_FILENO)) return std::nullopt;

    struct termios oldt;
    if (tcgetattr(STDIN_FILENO, &oldt) != 0) return std::nullopt;
    g_saved_termios = oldt;

    struct sigaction sa{};
    sa.sa_handler = restore_termios_and_raise;
    struct sigaction old_int, old_term;
    sigaction(SIGINT, &sa, &old_int);
    sigaction(SIGTERM, &sa, &old_term);

    struct termios newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    char c = 0;
    ssize_t n = read(STDIN_FILENO, &c, 1);

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    sigaction(SIGINT, &old_int, nullptr);
    sigaction(SIGTERM, &old_term, nullptr);

    if (n == 1) return c;
    return std::nullopt;
#endif
}

}} // namespace goose::cli
