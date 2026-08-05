#include "provider/openai.h"
#include "provider/provider_utils.h"
#include <spdlog/spdlog.h>
#include <sstream>
#include <map>

namespace goose {

OpenAiProvider::OpenAiProvider(std::string name, std::string base_url) {
    name_ = std::move(name);
    load_api_key(http_, "OPENAI_API_KEY");

    if (!base_url.empty()) {
        base_url_ = base_url + "/v1/chat/completions";
        return;
    }

    const char* base = std::getenv("OPENAI_BASE_URL");
    if (base) base_url_ = std::string(base) + "/v1/chat/completions";
}

nlohmann::json OpenAiProvider::build_request(
    const ModelConfig& model_config,
    const std::string& system_prompt,
    const std::vector<Message>& messages,
    const std::vector<Tool>& tools) {

    nlohmann::json request;
    request["model"] = model_config.model_name.empty() ? "gpt-4o" : model_config.model_name;
    request["stream"] = true;
    request["stream_options"] = {{"include_usage", true}};
    request["messages"] = nlohmann::json::array();

    if (!system_prompt.empty()) {
        request["messages"].push_back({{"role", "system"}, {"content", system_prompt}});
    }

    for (const auto& msg : messages) {
        nlohmann::json msg_json;

        if (msg.role == Role::Assistant) {
            if (msg.has_tool_requests()) {
                msg_json["role"] = "assistant";
                std::string text_content;
                nlohmann::json tool_calls = nlohmann::json::array();

                for (const auto& block : msg.content) {
                    if (auto* text = std::get_if<TextContent>(&block)) {
                        text_content += text->text;
                    } else if (auto* tr = std::get_if<ToolRequest>(&block)) {
                        if (!std::holds_alternative<CallToolRequestParams>(tr->tool_call)) continue;
                        const auto& params = std::get<CallToolRequestParams>(tr->tool_call);
                        tool_calls.push_back({
                            {"id", tr->id},
                            {"type", "function"},
                            {"function", {
                                {"name", params.name},
                                {"arguments", params.arguments.dump()}
                            }}
                        });
                    }
                }

                if (!text_content.empty()) msg_json["content"] = text_content;
                if (!tool_calls.empty()) msg_json["tool_calls"] = std::move(tool_calls);
            } else {
                msg_json["role"] = "assistant";
                msg_json["content"] = msg.as_concat_text();
            }
        } else if (msg.role == Role::User) {
            bool has_tool_responses = false;
            for (const auto& block : msg.content) {
                if (std::holds_alternative<ToolResponse>(block)) {
                    has_tool_responses = true;
                    break;
                }
            }

            if (has_tool_responses) {
                for (const auto& block : msg.content) {
                    if (auto* tr = std::get_if<ToolResponse>(&block)) {
                        nlohmann::json tool_msg;
                        tool_msg["role"] = "tool";
                        tool_msg["tool_call_id"] = tr->id;
                        if (std::holds_alternative<CallToolResult>(tr->tool_result)) {
                            const auto& result = std::get<CallToolResult>(tr->tool_result);
                            tool_msg["content"] = extract_tool_text(result);
                        } else {
                            tool_msg["content"] = std::get<std::string>(tr->tool_result);
                        }
                        request["messages"].push_back(std::move(tool_msg));
                    }
                }
                continue;
            } else {
                msg_json["role"] = "user";
                msg_json["content"] = msg.as_concat_text();
            }
        } else {
            msg_json["role"] = to_string(msg.role);
            msg_json["content"] = msg.as_concat_text();
        }

        if (!msg_json.empty()) {
            request["messages"].push_back(std::move(msg_json));
        }
    }

    if (!tools.empty()) {
        nlohmann::json tools_json = nlohmann::json::array();
        for (const auto& tool : tools) {
            tools_json.push_back({
                {"type", "function"},
                {"function", {
                    {"name", tool.name},
                    {"description", tool.description},
                    {"parameters", tool.input_schema}
                }}
            });
        }
        request["tools"] = std::move(tools_json);
    }

    if (model_config.max_tokens) {
        request["max_tokens"] = *model_config.max_tokens;
    }
    if (model_config.temperature) {
        request["temperature"] = *model_config.temperature;
    }
    if (!model_config.reasoning_effort.empty()) {
        request["reasoning_effort"] = openai_reasoning_effort(model_config.reasoning_effort);
    }

    return request;
}

Result<MessageStream>
OpenAiProvider::stream(
    const ModelConfig& model_config,
    const std::string& system_prompt,
    const std::vector<Message>& messages,
    const std::vector<Tool>& tools) {

    auto request = build_request(model_config, system_prompt, messages, tools);
    spdlog::debug("OpenAI stream request: {}", request.dump(2));

    struct StreamState {
        Message accumulated;
        ProviderUsage usage;
        std::map<int, nlohmann::json> tool_calls;
        std::map<int, std::string> tool_call_args;
        std::string current_reasoning;
        std::string current_content;
        bool finished = false;
        bool final_emitted = false;
        std::string final_error;
    };

    auto state = std::make_shared<StreamState>();
    state->accumulated = Message::assistant();
    state->accumulated.with_generated_id_if_missing();

    auto sse = http_.post_json_sse_async(base_url_, request);

    MessageStream stream = [sse, state]() mutable -> Result<StreamChunk> {
        if (state->final_emitted) {
            return Result<StreamChunk>::err(make_error(ErrorCode::StreamEnd, "done"));
        }

        // Consume lines until a text delta or the end of the stream is reached.
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

            nlohmann::json chunk;
            try {
                chunk = nlohmann::json::parse(data);
            } catch (...) {
                continue;
            }

            try {
                if (chunk.contains("error")) {
                    state->finished = true;
                    state->final_error = "OpenAI stream error: " + chunk["error"].dump();
                    break;
                }

                if (chunk.contains("choices") && !chunk["choices"].empty()) {
                    const auto& choice = chunk["choices"][0];
                    if (choice.contains("delta")) {
                        const auto& delta = choice["delta"];

                        if (delta.contains("reasoning_content") && delta["reasoning_content"].is_string()) {
                            state->current_reasoning += delta["reasoning_content"].get<std::string>();

                            if (!state->finished) {
                                StreamChunk partial;
                                Message partial_msg = Message::assistant();
                                if (!state->current_reasoning.empty()) {
                                    partial_msg.content.push_back(
                                        ThinkingContent{state->current_reasoning, ""});
                                }
                                partial.message = std::move(partial_msg);
                                return Result<StreamChunk>::ok(std::move(partial));
                            }
                        }

                        if (delta.contains("content") && delta["content"].is_string()) {
                            state->current_content += delta["content"].get<std::string>();

                            if (!state->finished) {
                                StreamChunk partial;
                                Message partial_msg = Message::assistant();
                                partial_msg.content.push_back(TextContent{state->current_content});
                                partial.message = std::move(partial_msg);
                                return Result<StreamChunk>::ok(std::move(partial));
                            }
                        }

                        if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                            for (const auto& tc : delta["tool_calls"]) {
                                int idx = tc.value("index", 0);

                                if (tc.contains("id") && tc.contains("function") &&
                                    tc["function"].contains("name") && tc["function"]["name"].is_string()) {
                                    state->tool_calls[idx] = nlohmann::json{
                                        {"id", tc["id"]},
                                        {"type", "function"},
                                        {"function", {{"name", tc["function"]["name"]}, {"arguments", ""}}}
                                    };
                                }

                                if (tc.contains("function") && tc["function"].contains("arguments") &&
                                    tc["function"]["arguments"].is_string()) {
                                    state->tool_call_args[idx] += tc["function"]["arguments"].get<std::string>();
                                }
                            }
                        }
                    }

                    if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
                        std::string reason = choice["finish_reason"].get<std::string>();
                        if (reason == "stop" || reason == "tool_calls") {
                            state->finished = true;
                        }
                    }
                }

                if (chunk.contains("usage")) {
                    const auto& u = chunk["usage"];
                    Usage usage = Usage::zero();
                    if (u.contains("prompt_tokens") && u["prompt_tokens"].is_number())
                        usage.input_tokens = u["prompt_tokens"].get<int64_t>();
                    if (u.contains("completion_tokens") && u["completion_tokens"].is_number())
                        usage.output_tokens = u["completion_tokens"].get<int64_t>();
                    if (u.contains("prompt_tokens_details") && u["prompt_tokens_details"].contains("cached_tokens") &&
                        u["prompt_tokens_details"]["cached_tokens"].is_number()) {
                        usage.cache_read_input_tokens = u["prompt_tokens_details"]["cached_tokens"].get<int64_t>();
                    }
                    state->usage = ProviderUsage{"openai", std::move(usage)};
                }
            } catch (const std::exception& e) {
                spdlog::warn("OpenAI stream parse error: {}", e.what());
            }
        }

        // The loop may exit early on [DONE] or finish_reason while the worker
        // is still finishing up. Drain remaining lines so `done` is set and
        // error/status_code below reflect the completed request.
        std::string line;
        while (sse->pop_line(line)) {
        }

        state->final_emitted = true;

        if (!state->final_error.empty()) {
            return Result<StreamChunk>::err(
                make_error(ErrorCode::ProviderError, state->final_error));
        }
        if (sse->error) {
            return Result<StreamChunk>::err(make_error(
                ErrorCode::NetworkError, sse->error_message));
        }
        if (sse->status_code != 200) {
            auto mapped = map_http_error(sse->status_code);
            return Result<StreamChunk>::err(mapped.error());
        }

        if (!state->current_reasoning.empty()) {
            state->accumulated.content.push_back(ThinkingContent{state->current_reasoning, ""});
        }

        if (!state->current_content.empty()) {
            state->accumulated.content.push_back(TextContent{state->current_content});
        }

        for (auto& [idx, tc_json] : state->tool_calls) {
            std::string args_str = state->tool_call_args.count(idx) ? state->tool_call_args[idx] : "{}";
            auto args = parse_tool_args(args_str);
            state->accumulated.content.push_back(
                make_tool_request(tc_json["id"], tc_json["function"]["name"], std::move(args)));
        }

        for (const auto& [idx, args_str] : state->tool_call_args) {
            if (state->tool_calls.count(idx)) continue;
            spdlog::error("Incomplete tool call (index {}): id/name lost in stream; tool NOT executed", idx);
            ToolRequest tr;
            tr.id = "incomplete_tool_call_" + std::to_string(idx);
            tr.tool_call = "Tool call incomplete: the stream ended before the tool call id/name arrived (index " +
                std::to_string(idx) + "); the tool was NOT executed. Retry the tool call if intended.";
            state->accumulated.content.push_back(std::move(tr));
        }

        StreamChunk chunk;
        chunk.message = state->accumulated;
        chunk.usage = state->usage;
        return Result<StreamChunk>::ok(std::move(chunk));
    };

    return Result<MessageStream>::ok(std::move(stream));
}

} // namespace goose
