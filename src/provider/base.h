#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>

#include "../core/types.h"
#include "../core/tool.h"
#include "model_config.h"
#include "provider_usage.h"

namespace goose {

// ============ 流式消息 ============

struct StreamChunk {
    std::optional<Message> message;
    std::optional<ProviderUsage> usage;
};

using MessageStream = std::function<Result<StreamChunk>()>;

// ============ Provider 基类 ============

class Provider {
public:
    virtual ~Provider() = default;

    virtual const std::string& get_name() const = 0;

    virtual Result<MessageStream>
    stream(const ModelConfig& model_config,
           const std::string& system_prompt,
           const std::vector<Message>& messages,
           const std::vector<Tool>& tools) = 0;

    // Non-streaming completion. Defaults to draining `stream`; providers may
    // override with a dedicated endpoint.
    virtual Result<Message>
    complete(const ModelConfig& model_config,
             const std::string& system_prompt,
             const std::vector<Message>& messages,
             const std::vector<Tool>& tools);
};

} // namespace goose
