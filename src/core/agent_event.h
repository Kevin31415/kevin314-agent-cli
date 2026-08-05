#pragma once

#include <optional>
#include <string>
#include "types.h"
#include "../provider/provider_usage.h"

namespace goose {

enum class AgentEventType {
    Message,
    TextDelta,
    ReasoningDelta,
    Usage,
};

struct AgentEvent {
    AgentEventType type;
    std::optional<Message> msg;
    std::optional<ProviderUsage> provider_usage;
    std::string text_delta;
    std::string reasoning_delta;

    static AgentEvent make_message(const Message& msg) {
        return AgentEvent{AgentEventType::Message, msg, std::nullopt, "", ""};
    }
    static AgentEvent make_text_delta(const std::string& delta) {
        return AgentEvent{AgentEventType::TextDelta, std::nullopt, std::nullopt, delta, ""};
    }
    static AgentEvent make_reasoning_delta(const std::string& delta) {
        return AgentEvent{AgentEventType::ReasoningDelta, std::nullopt, std::nullopt, "", delta};
    }
    static AgentEvent make_usage(const ProviderUsage& u) {
        return AgentEvent{AgentEventType::Usage, std::nullopt, u, "", ""};
    }
};

} // namespace goose
