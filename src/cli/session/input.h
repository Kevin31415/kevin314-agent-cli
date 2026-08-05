#pragma once

#include <string>
#include <functional>
#include <optional>

namespace goose {
namespace cli {

enum class InputResult {
    Message,
    SlashCommand,
    Retry,
    Exit
};

struct ParsedInput {
    InputResult type;
    std::string text;
    std::string command;
    std::vector<std::string> args;
};

ParsedInput parse_input(const std::string& raw);
std::string read_line(const std::string& prompt = "\n> ");

// Reads a single keypress without waiting for Enter. Returns nullopt when
// stdin is not a TTY or the read fails.
std::optional<char> read_key();

}} // namespace goose::cli
