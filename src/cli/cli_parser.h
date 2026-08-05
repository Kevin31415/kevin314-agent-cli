#pragma once

#include <string>
#include <vector>
#include <optional>
#include "../provider/model_config.h"
#include "../extension/extension_config.h"

namespace goose {
namespace cli {

struct CliArgs {
    std::string command = "session";

    bool resume = false;
    std::optional<std::string> name;
    std::optional<std::string> session_id;

    std::optional<std::string> instructions;
    std::optional<std::string> text;
    std::optional<std::string> recipe;
    std::optional<std::string> system_prompt;
    std::string output_format = "text";
    bool quiet = false;

    std::optional<std::string> provider;
    std::optional<std::string> model;
    bool debug = false;
    std::optional<uint32_t> max_turns;

    std::vector<std::string> extensions;

    bool session_delete = false;

    // template subcommand
    std::string template_action;
    std::string template_name;
    std::string template_content;

    // key subcommand
    std::optional<std::string> key_value;
    bool key_unset = false;
};

CliArgs parse_args(int argc, char* argv[]);

}} // namespace goose::cli
