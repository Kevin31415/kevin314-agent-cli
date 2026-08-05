#include "core/agent.h"
#include "config/config.h"
#include "core/compaction.h"
#include "core/moim.h"
#include "security/egress_inspector.h"
#include "security/permission_inspector.h"
#include "security/repetition_inspector.h"
#include "security/security_inspector.h"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace goose {

void Agent::rebuild_inspection_manager() {
    inspection_manager_ = std::make_unique<ToolInspectionManager>();
    // Inspectors run in registration order; later inspectors can only tighten
    // earlier decisions (Allow never overrides).
    inspection_manager_->add_inspector(std::make_unique<SecurityInspector>());
    inspection_manager_->add_inspector(std::make_unique<EgressInspector>());
    inspection_manager_->add_inspector(std::make_unique<PermissionInspector>(
        permission_manager_, provider_));
    inspection_manager_->add_inspector(std::make_unique<RepetitionInspector>(std::nullopt));
}

Agent::Agent(std::shared_ptr<Provider> provider, AgentConfig config,
             std::unique_ptr<ExtensionManager> extension_manager)
    : provider_(std::move(provider)), config_(std::move(config))
    , owned_extension_manager_(std::move(extension_manager))
    , permission_manager_(std::make_shared<PermissionManager>()) {
    if (!owned_extension_manager_) {
        owned_extension_manager_ = std::make_unique<ExtensionManager>();
    }
    extension_manager_ = owned_extension_manager_.get();
    rebuild_inspection_manager();
}

Agent::Agent(std::shared_ptr<Provider> provider, AgentConfig config,
             ExtensionManager* extension_manager)
    : provider_(std::move(provider)), config_(std::move(config))
    , extension_manager_(extension_manager)
    , permission_manager_(std::make_shared<PermissionManager>()) {
    if (!extension_manager_) {
        owned_extension_manager_ = std::make_unique<ExtensionManager>();
        extension_manager_ = owned_extension_manager_.get();
    }
    rebuild_inspection_manager();
}

Agent::~Agent() = default;

void Agent::interrupt() {
    interrupt_requested_.store(true);
}

bool Agent::interrupt_requested() const {
    return interrupt_requested_.load();
}

void Agent::apply_tool_annotations(const std::vector<Tool>& tools) {
    if (inspection_manager_) {
        inspection_manager_->apply_tool_annotations(tools);
    }
}

std::vector<Tool> Agent::list_tools() const {
    auto result = extension_manager_->list_tools();
    if (result) return *result;
    return {};
}

Result<void> Agent::reply(
    SessionConfig session_config,
    AgentEventCallback on_event,
    Conversation conversation) {

    interrupt_requested_.store(false);
    uint32_t turns_taken = 0;
    uint32_t max_turns = session_config.max_turns.value_or(1000);
    on_confirm_ = std::move(session_config.on_confirm);

    while (turns_taken < max_turns) {
        if (interrupt_requested_.load()) break;
        turns_taken++;

        double compaction_threshold = kDefaultCompactionThreshold;
        if (auto threshold = Config::global().get_param("GOOSE_AUTO_COMPACT_THRESHOLD"); threshold) {
            try {
                compaction_threshold = std::stod(*threshold);
            } catch (const std::exception&) {
            }
        }
        Conversation request_conv = inject_moim(
            conversation,
            config_.working_dir,
            model_config_.context_limit.value_or(kDefaultContextLimit),
            compaction_threshold,
            turns_taken,
            max_turns,
            estimate_conversation_tokens(conversation));

        auto stream_result = stream_response(request_conv, on_event);
        if (interrupt_requested_.load()) break;
        if (!stream_result) {
            return Result<void>::err(stream_result.error());
        }

        auto& assistant_response = stream_result->first;
        auto& messages_to_add = stream_result->second;

        if (!messages_to_add.empty()) {
            conversation.push(std::move(messages_to_add[0]));
            for (size_t i = 1; i < messages_to_add.size(); ++i) {
                on_event(AgentEvent::make_message(messages_to_add[i]));
                conversation.push(std::move(messages_to_add[i]));
            }
            continue;
        }

        on_event(AgentEvent::make_message(assistant_response));
        conversation.push(std::move(assistant_response));
        break;
    }

    return Result<void>::ok();
}

Result<std::pair<Message, std::vector<Message>>>
Agent::stream_response(Conversation& conversation, AgentEventCallback& on_event) {
    auto tools = list_tools();
    GooseMode mode = config_.session_manager
        ? Config::global().get_goose_mode()
        : GooseMode::Auto;
    std::string system_prompt = prompt_manager_.get_system_prompt(tools, mode, config_.working_dir, config_.additional_system_prompt);

    apply_tool_annotations(tools);

    auto fixed = fix_conversation(conversation);

    auto stream_result = provider_->stream(
        model_config_,
        system_prompt,
        fixed.messages(),
        tools);

    if (!stream_result) {
        return Result<std::pair<Message, std::vector<Message>>>::err(stream_result.error());
    }

    auto stream_fn = *stream_result;
    Message assistant_response;
    std::string accumulated_text;
    std::string accumulated_reasoning;

    while (true) {
        if (interrupt_requested_.load()) break;
        auto chunk_result = stream_fn();
        if (!chunk_result) {
            if (chunk_result.error().code == ErrorCode::StreamEnd) break;
            return Result<std::pair<Message, std::vector<Message>>>::err(chunk_result.error());
        }

        auto& chunk = chunk_result.value();
        if (chunk.message) {
            assistant_response = *chunk.message;

            std::string new_reasoning;
            for (const auto& block : assistant_response.content) {
                if (const auto* thinking = std::get_if<ThinkingContent>(&block)) {
                    new_reasoning += thinking->thinking;
                }
            }
            if (new_reasoning.size() > accumulated_reasoning.size()) {
                std::string delta = new_reasoning.substr(accumulated_reasoning.size());
                accumulated_reasoning = new_reasoning;
                on_event(AgentEvent::make_reasoning_delta(delta));
            }

            std::string new_text = assistant_response.as_concat_text();
            if (new_text.size() > accumulated_text.size()) {
                std::string delta = new_text.substr(accumulated_text.size());
                accumulated_text = new_text;
                on_event(AgentEvent::make_text_delta(delta));
            }
        }
        if (chunk.usage) {
            on_event(AgentEvent::make_usage(*chunk.usage));
        }
    }

    if (assistant_response.id.empty()) {
        assistant_response.with_generated_id_if_missing();
    }

    if (interrupt_requested_.load()) {
        return Result<std::pair<Message, std::vector<Message>>>::ok(
            std::make_pair(std::move(assistant_response), std::vector<Message>{}));
    }

    if (assistant_response.has_tool_requests()) {
        on_event(AgentEvent::make_message(assistant_response));

        std::vector<Message> messages_to_add;
        messages_to_add.push_back(assistant_response);

        auto tool_responses = execute_tool_calls(assistant_response, on_event);
        for (auto& msg : tool_responses) {
            messages_to_add.push_back(std::move(msg));
        }

        return Result<std::pair<Message, std::vector<Message>>>::ok(
            std::make_pair(std::move(assistant_response), std::move(messages_to_add)));
    }

    return Result<std::pair<Message, std::vector<Message>>>::ok(
        std::make_pair(std::move(assistant_response), std::vector<Message>{}));
}

std::vector<Message> Agent::execute_tool_calls(
    const Message& assistant_response, AgentEventCallback& on_event) {

    std::vector<Message> responses;

    GooseMode mode = config_.session_manager
        ? Config::global().get_goose_mode()
        : GooseMode::Auto;

    // Collect valid tool requests; surface malformed ones as feedback.
    std::vector<ToolRequest> requests;
    for (const auto& block : assistant_response.content) {
        if (!std::holds_alternative<ToolRequest>(block)) continue;

        const auto& tool_request = std::get<ToolRequest>(block);

        if (std::holds_alternative<std::string>(tool_request.tool_call)) {
            std::string err = std::get<std::string>(tool_request.tool_call);
            spdlog::error("Tool request contains error: {}", err);
            Message feedback = Message::user();
            feedback.id = "tool_error_feedback_" + tool_request.id;
            feedback.with_text("A tool call failed before execution (id: " + tool_request.id + "): " + err);
            responses.push_back(std::move(feedback));
            continue;
        }

        requests.push_back(tool_request);
    }

    if (requests.empty()) return responses;

    auto make_error_response = [&responses](const ToolRequest& request,
                                            const std::string& text) {
        CallToolResult call_result;
        call_result.is_error = true;
        nlohmann::json text_block;
        text_block["type"] = "text";
        text_block["text"] = text;
        call_result.content.push_back(std::move(text_block));

        ToolResponse tool_response;
        tool_response.id = request.id;
        tool_response.tool_result = std::move(call_result);

        Message tool_response_msg = Message::user();
        tool_response_msg.id = "tool_response_" + request.id;
        tool_response_msg.content.push_back(std::move(tool_response));
        responses.push_back(std::move(tool_response_msg));
    };

    if (mode == GooseMode::Chat) {
        for (const auto& request : requests) {
            make_error_response(request, "聊天模式下工具已被禁用");
        }
        return responses;
    }

    // Run the inspection pipeline: security -> egress -> permission -> repetition.
    auto inspection_results = inspection_manager_
        ? inspection_manager_->inspect_tools(requests, mode, &model_config_)
        : std::vector<InspectionResult>{};
    auto check = inspection_manager_
        ? inspection_manager_->process_inspection_results(requests, inspection_results)
        : PermissionCheckResult{};

    auto find_reason = [&inspection_results](const std::string& request_id) {
        for (const auto& r : inspection_results) {
            if (r.tool_request_id == request_id && r.action == InspectionAction::Deny) {
                return r.reason;
            }
        }
        return std::string();
    };

    for (const auto& request : check.approved) {
        if (interrupt_requested_.load()) break;
        const auto& params = std::get<CallToolRequestParams>(request.tool_call);
        spdlog::info("Executing tool: {}", params.name);
        auto tool_result = dispatch_tool_call(params, on_event);

        Message tool_response_msg = Message::user();
        tool_response_msg.id = "tool_response_" + request.id;

        ToolResponse tool_response;
        tool_response.id = request.id;

        if (tool_result) {
            CallToolResult call_result;
            nlohmann::json text_block;
            text_block["type"] = "text";
            text_block["text"] = tool_result->as_concat_text();
            call_result.content.push_back(std::move(text_block));
            tool_response.tool_result = std::move(call_result);
        } else {
            CallToolResult call_result;
            call_result.is_error = true;
            nlohmann::json text_block;
            text_block["type"] = "text";
            text_block["text"] = "错误: " + tool_result.error().message;
            call_result.content.push_back(std::move(text_block));
            tool_response.tool_result = std::move(call_result);
        }

        tool_response_msg.content.push_back(std::move(tool_response));
        responses.push_back(std::move(tool_response_msg));
    }

    for (const auto& request : check.denied) {
        const auto& params = std::get<CallToolRequestParams>(request.tool_call);
        spdlog::warn("Tool denied by inspection: {} ({})", params.name,
                     find_reason(request.id));
        make_error_response(request, "工具执行被拒绝: " + find_reason(request.id));
    }

    for (const auto& request : check.needs_approval) {
        const auto& params = std::get<CallToolRequestParams>(request.tool_call);
        if (on_confirm_ && on_confirm_(params.name, params.arguments)) {
            spdlog::info("Executing tool (approved): {}", params.name);
            auto tool_result = dispatch_tool_call(params, on_event);

            Message tool_response_msg = Message::user();
            tool_response_msg.id = "tool_response_" + request.id;

            ToolResponse tool_response;
            tool_response.id = request.id;

            if (tool_result) {
                CallToolResult call_result;
                nlohmann::json text_block;
                text_block["type"] = "text";
                text_block["text"] = tool_result->as_concat_text();
                call_result.content.push_back(std::move(text_block));
                tool_response.tool_result = std::move(call_result);
            } else {
                CallToolResult call_result;
                call_result.is_error = true;
                nlohmann::json text_block;
                text_block["type"] = "text";
                text_block["text"] = "错误: " + tool_result.error().message;
                call_result.content.push_back(std::move(text_block));
                tool_response.tool_result = std::move(call_result);
            }

            tool_response_msg.content.push_back(std::move(tool_response));
            responses.push_back(std::move(tool_response_msg));
        } else {
            make_error_response(request, "用户拒绝了工具执行");
        }
    }

    return responses;
}

Result<Message> Agent::dispatch_tool_call(
    const CallToolRequestParams& tool_call,
    AgentEventCallback& /*on_event*/) {

    auto result = extension_manager_->call_tool(tool_call.name, tool_call.arguments);
    if (!result) {
        return Result<Message>::err(result.error());
    }

    Message response = Message::assistant();
    response.with_text(result->dump());
    return Result<Message>::ok(std::move(response));
}

void Agent::add_extension(ExtensionConfig config) {
    extension_manager_->add_extension(std::move(config));
}

} // namespace goose
