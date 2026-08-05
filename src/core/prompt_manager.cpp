#include "prompt_manager.h"
#include "types.h"
#include "moim.h"
#include "../utils/prompt_template.h"
#include "../config/config.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <ctime>
#include <cstdlib>

namespace goose {

namespace {

constexpr int kMaxExtensions = 5;
constexpr int kMaxTools = 20;

std::string detect_os() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

std::string detect_shell() {
    const char* shell = std::getenv("SHELL");
    return shell && *shell ? shell : "sh";
}

std::string current_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    char time_buf[64];
#ifdef _WIN32
    struct tm tm_buf;
    localtime_s(&tm_buf, &time_t_now);
    std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
#else
    struct tm tm_buf;
    localtime_r(&time_t_now, &tm_buf);
    std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
#endif
    return time_buf;
}

std::string mode_behavior_text(GooseMode mode) {
    switch (mode) {
        case GooseMode::Auto:
            return "Tools will be executed automatically without confirmation.";
        case GooseMode::Approve:
            return "All tool calls require user approval before execution.";
        case GooseMode::SmartApprove:
            return "Safe tools execute automatically; risky tools require approval.";
        case GooseMode::Chat:
            return "You are in chat mode. Do not use tools.";
        case GooseMode::BypassPermissions:
            return "All permission checks are bypassed.";
    }
    return "";
}

nlohmann::json tools_to_extensions(const std::vector<Tool>& tools) {
    nlohmann::json extensions = nlohmann::json::array();
    for (const auto& tool : tools) {
        extensions.push_back({
            {"name", tool.name},
            {"has_resources", false},
            {"instructions", tool.description},
        });
    }
    return extensions;
}

} // namespace

std::string PromptManager::get_system_prompt(
    const std::vector<Tool>& tools,
    GooseMode mode,
    const std::string& working_dir,
    const std::optional<std::string>& additional_prompt) const {

    const std::string& template_name = Config::global().get_system_prompt_template();

    nlohmann::json ctx;
    if (template_name == "tiny_model_system.md") {
        ctx["os"] = detect_os();
        ctx["shell"] = detect_shell();
        ctx["working_directory"] = working_dir;
    } else {
        ctx["moim_system_prompt_block"] = system_prompt_block();
        ctx["code_execution_mode"] = false;
        ctx["extensions"] = tools_to_extensions(tools);
        ctx["mode"] = to_string(mode);
        ctx["mode_behavior"] = mode_behavior_text(mode);
        ctx["working_directory"] = working_dir;
        ctx["current_time"] = current_timestamp();
        if (additional_prompt && !additional_prompt->empty()) {
            ctx["additional_instructions"] = *additional_prompt;
        }
        if (tools.size() > kMaxTools) {
            ctx["extension_tool_limits"] =
                nlohmann::json::array({tools.size(), tools.size()});
            ctx["max_extensions"] = kMaxExtensions;
            ctx["max_tools"] = kMaxTools;
        }
    }

    std::string rendered = PromptTemplate::global().render(template_name, ctx);
    if (!rendered.empty()) {
        return rendered;
    }
    spdlog::warn("System prompt template '{}' rendered empty", template_name);
    return "";
}

} // namespace goose
