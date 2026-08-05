#include "provider/anthropic.h"
#include "provider/provider_utils.h"
#include <spdlog/spdlog.h>
#include <map>

namespace goose {

AnthropicProvider::AnthropicProvider(std::string name, std::string base_url) {
    name_ = std::move(name);
    load_api_key(http_, "ANTHROPIC_API_KEY");

    if (!base_url.empty()) {
        base_url_ = base_url;
    } else {
        const char* base = std::getenv("ANTHROPIC_BASE_URL");
        if (base) base_url_ = std::string(base);
    }

    http_.set_extra_headers({"anthropic-version: 2023-06-01"});
}

nlohmann::json AnthropicProvider::build_request(
    const ModelConfig& model_config,
    const std::string& system_prompt,
    const std::vector<Message>& messages,
    const std::vector<Tool>& tools) {

    nlohmann::json request;
    request["model"] = model_config.model_name.empty() ? "claude-sonnet-4-20250514" : model_config.model_name;
    request["stream"] = true;
    request["max_tokens"] = model_config.max_tokens.value_or(8192);

    if (const int budget = anthropic_thinking_budget(model_config.reasoning_effort); budget > 0) {
        // 启用扩展思考；budget_tokens 必须小于 max_tokens，不足时上调。
        if (request["max_tokens"].get<int>() <= budget) {
            request["max_tokens"] = budget + 1024;
        }
        request["thinking"] = {{"type", "enabled"}, {"budget_tokens", budget}};
    }

    if (!system_prompt.empty()) {
        request["system"] = nlohmann::json::array({
            {{"type", "text"}, {"text", system_prompt}}
        });
    }

    request["messages"] = nlohmann::json::array();
    for (const auto& msg : messages) {
        nlohmann::json msg_json;
        msg_json["role"] = (msg.role == Role::Assistant) ? "assistant" : "user";
        msg_json["content"] = nlohmann::json::array();

        for (const auto& block : msg.content) {
            if (auto* text = std::get_if<TextContent>(&block)) {
                if (!text->text.empty()) {
                    msg_json["content"].push_back({{"type", "text"}, {"text", text->text}});
                }
            } else if (auto* tr = std::get_if<ToolRequest>(&block)) {
                if (std::holds_alternative<CallToolRequestParams>(tr->tool_call)) {
                    const auto& params = std::get<CallToolRequestParams>(tr->tool_call);
                    msg_json["content"].push_back({
                        {"type", "tool_use"},
                        {"id", tr->id},
                        {"name", params.name},
                        {"input", params.arguments}
                    });
                }
            } else if (auto* resp = std::get_if<ToolResponse>(&block)) {
                if (std::holds_alternative<CallToolResult>(resp->tool_result)) {
                    const auto& result = std::get<CallToolResult>(resp->tool_result);
                    nlohmann::json tool_result;
                    tool_result["type"] = "tool_result";
                    tool_result["tool_use_id"] = resp->id;
                    if (result.is_error) {
                        tool_result["is_error"] = true;
                    }
                    if (!result.content.empty()) {
                        // A tool result can carry multiple content blocks;
                        // Anthropic accepts a list, so preserve them all.
                        nlohmann::json blocks = nlohmann::json::array();
                        for (const auto& block : result.content) {
                            if (block.contains("text")) {
                                blocks.push_back(block["text"]);
                            } else {
                                blocks.push_back(block.dump());
                            }
                        }
                        tool_result["content"] = std::move(blocks);
                    }
                    msg_json["content"].push_back(std::move(tool_result));
                } else {
                    nlohmann::json tool_result;
                    tool_result["type"] = "tool_result";
                    tool_result["tool_use_id"] = resp->id;
                    tool_result["is_error"] = true;
                    tool_result["content"] = std::get<std::string>(resp->tool_result);
                    msg_json["content"].push_back(std::move(tool_result));
                }
            }
        }

        if (msg_json["content"].empty()) {
            msg_json["content"].push_back({{"type", "text"}, {"text", " "}});
        }

        request["messages"].push_back(std::move(msg_json));
    }

    if (!tools.empty()) {
        nlohmann::json tools_json = nlohmann::json::array();
        for (const auto& tool : tools) {
            tools_json.push_back({
                {"name", tool.name},
                {"description", tool.description},
                {"input_schema", tool.input_schema}
            });
        }
        request["tools"] = std::move(tools_json);
    }

    if (model_config.temperature) {
        request["temperature"] = *model_config.temperature;
    }

    return request;
}

Result<MessageStream>
AnthropicProvider::stream(
    const ModelConfig& model_config,
    const std::string& system_prompt,
    const std::vector<Message>& messages,
    const std::vector<Tool>& tools) {

    auto request = build_request(model_config, system_prompt, messages, tools);
    spdlog::debug("Anthropic stream request: {}", request.dump(2));

    struct StreamState {
        Message accumulated;
        ProviderUsage usage;
        std::string total_text;
        std::string block_text;
        std::string current_tool_args;
        std::string current_tool_id;
        std::string current_tool_name;
        bool in_tool = false;
        bool finished = false;
        bool final_emitted = false;
    };

    auto state = std::make_shared<StreamState>();
    state->accumulated = Message::assistant();
    state->accumulated.with_generated_id_if_missing();

    std::string url = base_url_ + "/v1/messages";
    auto sse = http_.post_json_sse_async(url, request);

    MessageStream stream = [sse, state]() mutable -> Result<StreamChunk> {
        if (state->final_emitted) {
            return Result<StreamChunk>::err(make_error(ErrorCode::StreamEnd, "done"));
        }

        while (true) {
            std::string line;
            if (!sse->pop_line(line)) {
                state->finished = true;
                break;
            }

            std::string data = strip_sse_data(line);
            if (data.empty()) continue;

            if (data == "[DONE]") {
                state->finished = true;
                break;
            }

            nlohmann::json event;
            try {
                event = nlohmann::json::parse(data);
            } catch (...) {
                continue;
            }

            std::string type = event.value("type", "");

            if (type == "message_start") {
                if (event.contains("message") && event["message"].contains("usage")) {
                    const auto& u = event["message"]["usage"];
                    Usage usage = Usage::zero();
                    usage.input_tokens = u.value("input_tokens", 0);
                    state->usage = ProviderUsage{"anthropic", std::move(usage)};
                }
            } else if (type == "content_block_start") {
                if (event.contains("content_block")) {
                    const auto& block = event["content_block"];
                    std::string btype = block.value("type", "");
                    if (btype == "tool_use") {
                        state->in_tool = true;
                        state->current_tool_id = block.value("id", "");
                        state->current_tool_name = block.value("name", "");
                        state->current_tool_args.clear();
                    }
                }
            } else if (type == "content_block_delta") {
                if (event.contains("delta")) {
                    const auto& delta = event["delta"];
                    std::string dtype = delta.value("type", "");
                    if (dtype == "text_delta") {
                        state->block_text += delta.value("text", "");
                        state->total_text += delta.value("text", "");

                        if (!state->finished) {
                            StreamChunk partial;
                            Message partial_msg = Message::assistant();
                            partial_msg.content.push_back(TextContent{state->total_text});
                            partial.message = std::move(partial_msg);
                            return Result<StreamChunk>::ok(std::move(partial));
                        }
                    } else if (dtype == "input_json_delta") {
                        state->current_tool_args += delta.value("partial_json", "");
                    }
                }
            } else if (type == "content_block_stop") {
                if (state->in_tool) {
                    state->in_tool = false;

                    if (state->current_tool_id.empty() || state->current_tool_name.empty()) {
                        spdlog::warn("Dropping incomplete Anthropic tool call: missing id or name");
                    } else {
                        auto args = parse_tool_args(state->current_tool_args);
                        state->accumulated.content.push_back(
                            make_tool_request(state->current_tool_id, state->current_tool_name, std::move(args)));
                    }

                    state->current_tool_id.clear();
                    state->current_tool_name.clear();
                    state->current_tool_args.clear();
                } else if (!state->block_text.empty()) {
                    state->accumulated.content.push_back(TextContent{state->block_text});
                    state->block_text.clear();
                }
            } else if (type == "message_delta") {
                if (event.contains("delta") && event["delta"].contains("stop_reason")) {
                    state->finished = true;
                }
                if (event.contains("usage")) {
                    const auto& u = event["usage"];
                    if (state->usage.usage.output_tokens == std::nullopt) {
                        state->usage.usage.output_tokens = u.value("output_tokens", 0);
                    }
                }
            } else if (type == "message_stop") {
                state->finished = true;
            }
        }

        // message_stop can arrive before the worker finishes; drain so
        // error/status_code below reflect the completed request.
        std::string line;
        while (sse->pop_line(line)) {
        }

        state->final_emitted = true;

        if (sse->error) {
            return Result<StreamChunk>::err(make_error(
                ErrorCode::NetworkError, sse->error_message));
        }
        if (sse->status_code != 200) {
            auto mapped = map_http_error(sse->status_code);
            return Result<StreamChunk>::err(mapped.error());
        }

        if (!state->block_text.empty()) {
            state->accumulated.content.push_back(TextContent{state->block_text});
            state->block_text.clear();
        }

        StreamChunk chunk;
        chunk.message = state->accumulated;
        chunk.usage = state->usage;
        return Result<StreamChunk>::ok(std::move(chunk));
    };

    return Result<MessageStream>::ok(std::move(stream));
}

} // namespace goose
