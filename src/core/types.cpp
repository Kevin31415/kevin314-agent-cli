#include "core/types.h"
#include "core/tool.h"
#include "provider/model_config.h"
#include "provider/provider_usage.h"
#include <nlohmann/json.hpp>
#include <random>
#include <sstream>
#include <iomanip>

namespace goose {

// ============ UUID 生成 ============

static std::string generate_uuid() {
    thread_local std::mt19937 gen(std::random_device{}());
    thread_local std::uniform_int_distribution<uint32_t> dis(0, 0xFFFFFFFF);

    std::ostringstream oss;
    for (int i = 0; i < 8; i++) oss << std::hex << dis(gen);
    return oss.str();
}

// ============ Message ============

Message Message::user() {
    Message m;
    m.id = generate_uuid();
    m.role = Role::User;
    m.created_at = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    return m;
}

Message Message::assistant() {
    Message m;
    m.id = generate_uuid();
    m.role = Role::Assistant;
    m.created_at = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    return m;
}

Message& Message::with_text(const std::string& text) {
    content.push_back(TextContent{text});
    return *this;
}

Message& Message::with_generated_id_if_missing() {
    if (id.empty()) id = generate_uuid();
    return *this;
}

std::string Message::as_concat_text() const {
    std::string result;
    for (const auto& block : content) {
        if (auto* text = std::get_if<TextContent>(&block)) {
            result += text->text;
        }
    }
    return result;
}

bool Message::has_tool_requests() const {
    for (const auto& block : content) {
        if (std::holds_alternative<ToolRequest>(block)) return true;
    }
    return false;
}

// ============ Conversation ============

Conversation::Conversation(std::vector<Message> messages)
    : messages_(std::move(messages)) {}

void Conversation::push(Message message) {
    messages_.push_back(std::move(message));
}

// ============ Message JSON 序列化 ============

static void to_json_content_block(nlohmann::json& j, const MessageContentBlock& block) {
    std::visit([&](const auto& content) {
        using T = std::decay_t<decltype(content)>;
        if constexpr (std::is_same_v<T, TextContent>) {
            j = {{"type", "text"}, {"text", content.text}};
        } else if constexpr (std::is_same_v<T, ImageContent>) {
            j = {{"type", "image"}, {"mimeType", content.mime_type}, {"data", content.data}};
        } else if constexpr (std::is_same_v<T, ToolRequest>) {
            nlohmann::json tc;
            if (std::holds_alternative<CallToolRequestParams>(content.tool_call)) {
                const auto& params = std::get<CallToolRequestParams>(content.tool_call);
                tc = {{"name", params.name}, {"arguments", params.arguments}};
            } else {
                tc = {{"error", std::get<std::string>(content.tool_call)}};
            }
            j = {{"type", "toolRequest"}, {"id", content.id}, {"toolCall", tc}};
        } else if constexpr (std::is_same_v<T, ToolResponse>) {
            nlohmann::json tr;
            if (std::holds_alternative<CallToolResult>(content.tool_result)) {
                const auto& result = std::get<CallToolResult>(content.tool_result);
                tr = {{"content", result.content}, {"isError", result.is_error}};
            } else {
                tr = {{"error", std::get<std::string>(content.tool_result)}};
            }
            j = {{"type", "toolResponse"}, {"id", content.id}, {"toolResult", tr}};
        } else if constexpr (std::is_same_v<T, ThinkingContent>) {
            j = {{"type", "thinking"}, {"thinking", content.thinking}, {"signature", content.signature}};
        } else if constexpr (std::is_same_v<T, RedactedThinkingContent>) {
            j = {{"type", "redactedThinking"}, {"data", content.data}};
        } else if constexpr (std::is_same_v<T, SystemNotificationContent>) {
            j = {{"type", "systemNotification"}, {"notificationType", content.notification_type}, {"msg", content.msg}};
        } else if constexpr (std::is_same_v<T, ToolConfirmationRequest>) {
            j = {{"type", "toolConfirmationRequest"}, {"id", content.id}, {"toolName", content.tool_name},
                 {"arguments", content.arguments}};
            if (content.prompt) j["prompt"] = *content.prompt;
        }
    }, block);
}

static MessageContentBlock content_block_from_json(const nlohmann::json& j) {
    std::string type = j.value("type", "");
    if (type == "text") {
        TextContent tc;
        tc.text = j.at("text").get<std::string>();
        return tc;
    } else if (type == "image") {
        ImageContent ic;
        ic.mime_type = j.at("mimeType").get<std::string>();
        ic.data = j.at("data").get<std::string>();
        return ic;
    } else if (type == "toolRequest") {
        ToolRequest tr;
        tr.id = j.at("id").get<std::string>();
        const auto& tc = j.at("toolCall");
        if (tc.contains("error")) {
            tr.tool_call = tc.at("error").get<std::string>();
        } else {
            CallToolRequestParams params;
            params.name = tc.at("name").get<std::string>();
            params.arguments = tc.value("arguments", nlohmann::json{});
            tr.tool_call = std::move(params);
        }
        return tr;
    } else if (type == "toolResponse") {
        ToolResponse tr;
        tr.id = j.at("id").get<std::string>();
        const auto& trj = j.at("toolResult");
        if (trj.contains("error")) {
            tr.tool_result = trj.at("error").get<std::string>();
        } else {
            CallToolResult cr;
            cr.content = trj.value("content", std::vector<nlohmann::json>{});
            cr.is_error = trj.value("isError", false);
            tr.tool_result = std::move(cr);
        }
        return tr;
    } else if (type == "thinking") {
        ThinkingContent tc;
        tc.thinking = j.at("thinking").get<std::string>();
        tc.signature = j.value("signature", "");
        return tc;
    } else if (type == "redactedThinking") {
        RedactedThinkingContent rtc;
        rtc.data = j.at("data").get<std::string>();
        return rtc;
    } else if (type == "systemNotification") {
        SystemNotificationContent snc;
        snc.notification_type = j.value("notificationType", "");
        snc.msg = j.value("msg", "");
        return snc;
    } else if (type == "toolConfirmationRequest") {
        ToolConfirmationRequest tcr;
        tcr.id = j.at("id").get<std::string>();
        tcr.tool_name = j.at("toolName").get<std::string>();
        tcr.arguments = j.value("arguments", nlohmann::json::object());
        if (j.contains("prompt")) tcr.prompt = j.at("prompt").get<std::string>();
        return tcr;
    }
    return TextContent{"[unknown content block type: " + type + "]"};
}

void to_json(nlohmann::json& j, const Message& m) {
    j["id"] = m.id;
    j["role"] = to_string(m.role);
    j["created"] = m.created_at;

    nlohmann::json content_arr = nlohmann::json::array();
    for (const auto& block : m.content) {
        nlohmann::json block_json;
        to_json_content_block(block_json, block);
        content_arr.push_back(std::move(block_json));
    }
    j["content"] = std::move(content_arr);

    j["metadata"] = {
        {"userVisible", m.metadata.user_visible},
        {"agentVisible", m.metadata.agent_visible}
    };
    if (m.metadata.inference) {
        j["metadata"]["inference"] = *m.metadata.inference;
    }
}

void from_json(const nlohmann::json& j, Message& m) {
    m.id = j.value("id", "");
    m.role = role_from_string(j.value("role", "user"));
    m.created_at = j.value("created", 0);

    if (j.contains("content") && j["content"].is_array()) {
        for (const auto& block_json : j["content"]) {
            m.content.push_back(content_block_from_json(block_json));
        }
    }

    if (j.contains("metadata")) {
        const auto& meta = j["metadata"];
        m.metadata.user_visible = meta.value("userVisible", true);
        m.metadata.agent_visible = meta.value("agentVisible", true);
        if (meta.contains("inference")) {
            m.metadata.inference = meta["inference"];
        }
    }
}

void to_json(nlohmann::json& j, const Conversation& c) {
    j = nlohmann::json::array();
    for (const auto& msg : c.messages()) {
        nlohmann::json msg_json;
        to_json(msg_json, msg);
        j.push_back(std::move(msg_json));
    }
}

void from_json(const nlohmann::json& j, Conversation& c) {
    if (j.is_array()) {
        for (const auto& msg_json : j) {
            Message msg;
            from_json(msg_json, msg);
            c.push(std::move(msg));
        }
    }
}

// ============ 对话修复与过滤 ============

Conversation fix_conversation(const Conversation& conv) {
    Conversation fixed;
    const auto& msgs = conv.messages();
    if (msgs.empty()) return fixed;

    for (size_t i = 0; i < msgs.size(); ++i) {
        const auto& msg = msgs[i];

        if (fixed.len() > 0) {
            auto& last = const_cast<std::vector<Message>&>(fixed.messages()).back();
            if (last.role == msg.role && !msg.has_tool_requests() && !last.has_tool_requests()) {
                for (const auto& block : msg.content) {
                    last.content.push_back(block);
                }
                continue;
            }
        }

        if (msg.role == Role::Assistant && fixed.len() > 0) {
            const auto& prev = fixed.messages().back();
            if (prev.role == Role::Assistant) {
                fixed.push(msg);
                continue;
            }
        }

        if (msg.role == Role::User) {
            fixed.push(msg);
        } else {
            if (fixed.len() == 0) {
                Message filler = Message::user().with_text("");
                filler.metadata.user_visible = false;
                filler.metadata.agent_visible = false;
                fixed.push(std::move(filler));
            }
            fixed.push(msg);
        }
    }

    return fixed;
}

} // namespace goose
