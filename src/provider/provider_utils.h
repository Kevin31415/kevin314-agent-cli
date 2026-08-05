#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "../utils/error.h"
#include "../core/types.h"
#include "http_client.h"
#include "model_config.h"
#include "provider_usage.h"
#include "../config/secrets.h"

namespace goose {

inline void load_api_key(HttpClient& http, const std::string& secret_name) {
    auto key = Secrets::global().get_secret(secret_name);
    if (key) {
        http.set_auth_token(*key);
    } else {
        const char* api_key = std::getenv(secret_name.c_str());
        if (api_key) {
            std::string key_copy(api_key);
            http.set_auth_token(key_copy);
        }
    }
}

inline std::string strip_sse_data(const std::string& line) {
    if (line.substr(0, 6) == "data: ") {
        return line.substr(6);
    } else if (line.substr(0, 5) == "data:") {
        return line.substr(5);
    }
    return "";
}

inline std::string extract_tool_text(const CallToolResult& result) {
    std::string text;
    for (const auto& block : result.content) {
        if (block.contains("text")) {
            text += block["text"].get<std::string>();
        } else {
            text += block.dump();
        }
    }
    return text;
}

inline nlohmann::json parse_tool_args(const std::string& args_str) {
    nlohmann::json args;
    try {
        args = nlohmann::json::parse(args_str);
        if (!args.is_object()) args = nlohmann::json::object();
    } catch (...) {
        args = nlohmann::json::object();
    }
    return args;
}

// 抽象思考等级（low/medium/high/max）→ OpenAI reasoning_effort。
// OpenAI 官方只接受 low/medium/high，max 落到最高档 high；其它值透传，
// 由兼容接口自行判断（部分服务商支持 xhigh 等自有档位）。
inline std::string openai_reasoning_effort(const std::string& level) {
    if (level == "max") return "high";
    return level;
}

// 抽象思考等级 → Anthropic thinking budget_tokens；返回 0 表示不启用思考。
// Anthropic 用 token 预算而非命名档位，这里做保守映射。
inline int anthropic_thinking_budget(const std::string& level) {
    if (level == "low") return 4096;
    if (level == "medium") return 8192;
    if (level == "high") return 16384;
    if (level == "max") return 32000;
    return 0;
}

inline ToolRequest make_tool_request(const std::string& id,
                                      const std::string& name,
                                      nlohmann::json args) {
    CallToolRequestParams params;
    params.name = name;
    params.arguments = std::move(args);
    ToolRequest tr;
    tr.id = id;
    tr.tool_call = std::move(params);
    return tr;
}

inline Result<MessageStream> map_http_error(int status) {
    if (status == 0) {
        return Result<MessageStream>::err(make_error(
            ErrorCode::NetworkError,
            "无法连接到服务器或连接中断（未收到 HTTP 响应），请检查网络/代理设置或稍后重试"));
    }
    switch (status) {
        case 401:
        case 403:
            return Result<MessageStream>::err(
                make_error(ErrorCode::AuthError, "认证失败 (HTTP " + std::to_string(status) + ")"));
        case 402:
            return Result<MessageStream>::err(
                make_error(ErrorCode::CreditsExhaustedError, "余额不足，请充值后重试"));
        case 404:
            return Result<MessageStream>::err(
                make_error(ErrorCode::ProviderError, "资源不存在 (HTTP 404)"));
        case 413:
            return Result<MessageStream>::err(
                make_error(ErrorCode::ContextLengthError, "上下文长度超出限制 (HTTP 413)"));
        case 429:
            return Result<MessageStream>::err(
                make_error(ErrorCode::RateLimitError, "请求频率超限 (HTTP 429)"));
        default:
            if (status >= 500) {
                return Result<MessageStream>::err(
                    make_error(ErrorCode::ServerError, "服务器错误 (HTTP " + std::to_string(status) + ")"));
            }
            return Result<MessageStream>::err(
                make_error(ErrorCode::ProviderError, "HTTP " + std::to_string(status)));
    }
}

} // namespace goose
