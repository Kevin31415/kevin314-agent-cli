#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <functional>
#include <nlohmann/json.hpp>

#include "types.h"
#include "agent_config.h"
#include "agent_event.h"
#include "tool.h"
#include "prompt_manager.h"
#include "../provider/base.h"
#include "../provider/model_config.h"
#include "../extension/extension_manager.h"
#include "../security/permission_manager.h"
#include "../security/tool_inspection.h"

namespace goose {

struct SessionConfig {
    std::optional<uint32_t> max_turns;
    std::function<bool(const std::string& tool_name, const nlohmann::json& args)> on_confirm;
};

using AgentEventCallback = std::function<void(AgentEvent)>;

class Agent {
public:
    Agent(std::shared_ptr<Provider> provider, AgentConfig config,
          std::unique_ptr<ExtensionManager> extension_manager = nullptr);
    Agent(std::shared_ptr<Provider> provider, AgentConfig config,
          ExtensionManager* extension_manager);
    ~Agent();

    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;
    Agent(Agent&&) = delete;
    Agent& operator=(Agent&&) = delete;

    // Runs the agent loop over the given conversation. The conversation must
    // already contain the latest user message; the caller owns message
    // persistence for messages it pushes.
    Result<void> reply(
        SessionConfig session_config,
        AgentEventCallback on_event,
        Conversation conversation);

    std::shared_ptr<Provider> provider() const { return provider_; }
    const AgentConfig& config() const { return config_; }
    void set_model_config(const ModelConfig& mc) { model_config_ = mc; }
    const ModelConfig& model_config() const { return model_config_; }
    void set_provider(std::shared_ptr<Provider> p) {
        provider_ = std::move(p);
        rebuild_inspection_manager();
    }
    void add_extension(ExtensionConfig config);

    std::vector<Tool> list_tools() const;

    // 请求中断当前正在进行的回复/工具执行（线程安全，可随时调用）。
    void interrupt();
    bool interrupt_requested() const;

private:
    Result<std::pair<Message, std::vector<Message>>>
    stream_response(Conversation& conversation, AgentEventCallback& on_event);

    std::vector<Message> execute_tool_calls(
        const Message& assistant_response, AgentEventCallback& on_event);

    Result<Message> dispatch_tool_call(
        const CallToolRequestParams& tool_call,
        AgentEventCallback& on_event);

    std::shared_ptr<Provider> provider_;
    AgentConfig config_;
    ExtensionManager* extension_manager_;
    std::unique_ptr<ExtensionManager> owned_extension_manager_;
    PromptManager prompt_manager_;
    ModelConfig model_config_;
    std::function<bool(const std::string&, const nlohmann::json&)> on_confirm_;

    std::shared_ptr<PermissionManager> permission_manager_;
    std::unique_ptr<ToolInspectionManager> inspection_manager_;
    std::atomic<bool> interrupt_requested_{false};
    void rebuild_inspection_manager();
    void apply_tool_annotations(const std::vector<Tool>& tools);
};

} // namespace goose
