#pragma once

#include <string>
#include <vector>
#include <variant>
#include <optional>
#include <cstdint>
#include <nlohmann/json.hpp>
#include "../utils/error.h"

namespace goose {

// ============ 基础类型 ============

enum class Role { User, Assistant };

inline const char* to_string(Role r) {
    return r == Role::User ? "user" : "assistant";
}

inline Role role_from_string(const std::string& s) {
    return s == "assistant" ? Role::Assistant : Role::User;
}

// ============ Content 类型 ============



struct TextContent {
    std::string text;
};

struct ImageContent {
    std::string mime_type;
    std::string data;
};

struct ThinkingContent {
    std::string thinking;
    std::string signature;
};

struct RedactedThinkingContent {
    std::string data;
};

struct CallToolRequestParams {
    std::string name;
    nlohmann::json arguments;
};

struct CallToolResult {
    std::vector<nlohmann::json> content;
    bool is_error = false;
};

struct ToolRequest {
    std::string id;
    std::variant<CallToolRequestParams, std::string> tool_call;
};

struct ToolResponse {
    std::string id;
    std::variant<CallToolResult, std::string> tool_result;
};

struct ToolConfirmationRequest {
    std::string id;
    std::string tool_name;
    nlohmann::json arguments;
    std::optional<std::string> prompt;
};

struct SystemNotificationContent {
    std::string notification_type;
    std::string msg;
};

// ============ MessageContentBlock ============

using MessageContentBlock = std::variant<
    TextContent,
    ImageContent,
    ToolRequest,
    ToolResponse,
    ToolConfirmationRequest,
    ThinkingContent,
    RedactedThinkingContent,
    SystemNotificationContent
>;

// ============ Message 元数据 ============

struct MessageMetadata {
    bool user_visible = true;
    bool agent_visible = true;
    std::optional<nlohmann::json> inference;
};

// ============ Message ============

struct Message {
    std::string id;
    Role role;
    std::vector<MessageContentBlock> content;
    MessageMetadata metadata;
    int64_t created_at = 0;

    // Builder pattern
    static Message user();
    static Message assistant();

    Message& with_text(const std::string& text);
    Message& with_generated_id_if_missing();

    // 提取文本内容
    std::string as_concat_text() const;

    // 查询方法
    bool has_tool_requests() const;
};

// ============ Conversation ============

class Conversation {
public:
    Conversation() = default;
    explicit Conversation(std::vector<Message> messages);

    void push(Message message);
    const std::vector<Message>& messages() const { return messages_; }

    size_t len() const { return messages_.size(); }
    bool is_empty() const { return messages_.empty(); }

private:
    std::vector<Message> messages_;
};

// ============ JSON 序列化 ============

void to_json(nlohmann::json& j, const Message& m);
void from_json(const nlohmann::json& j, Message& m);
void to_json(nlohmann::json& j, const Conversation& c);
void from_json(const nlohmann::json& j, Conversation& c);

// ============ 对话修复 ============

Conversation fix_conversation(const Conversation& conv);

} // namespace goose
