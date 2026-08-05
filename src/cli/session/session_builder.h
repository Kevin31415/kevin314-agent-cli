#pragma once

#include <string>
#include <optional>
#include <vector>
#include "../../core/agent.h"
#include "../../provider/base.h"
#include "../../extension/extension_manager.h"
#include "../../session/session_manager.h"
#include "cli_session.h"

namespace goose {
namespace cli {

struct SessionBuilderConfig {
    std::optional<std::string> session_id;
    bool resume = false;
    std::optional<std::string> name;
    std::optional<std::string> provider_name;
    std::optional<std::string> model_name;
    std::vector<std::string> extensions;
    std::optional<std::string> additional_system_prompt;
    bool debug = false;
    int max_turns = 0;
    bool quiet = false;
    std::optional<std::string> text;
    std::string output_format = "text";
};

Result<CliSession> build_session(const SessionBuilderConfig& config);

}} // namespace goose::cli
