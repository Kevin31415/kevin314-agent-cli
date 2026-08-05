#include "core/compaction.h"

#include <algorithm>

namespace goose {

namespace {

// ============ StructuredSummary lenient parsing ============

std::string stringify_lenient(const nlohmann::json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_null()) return "";
    if (value.is_object()) {
        std::vector<std::string> parts;
        for (auto it = value.begin(); it != value.end(); ++it) {
            parts.push_back(it.key() + ": " + stringify_lenient(it.value()));
        }
        std::string joined;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i) joined += "; ";
            joined += parts[i];
        }
        return joined;
    }
    if (value.is_array()) {
        std::vector<std::string> parts;
        for (const auto& item : value) parts.push_back(stringify_lenient(item));
        std::string joined;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i) joined += "; ";
            joined += parts[i];
        }
        return joined;
    }
    return value.dump();
}

std::vector<std::string> lenient_string_list(const nlohmann::json& value) {
    std::vector<std::string> out;
    if (value.is_array()) {
        for (const auto& item : value) out.push_back(stringify_lenient(item));
    } else if (!value.is_null()) {
        out.push_back(stringify_lenient(value));
    }
    return out;
}

std::vector<FileActivity> lenient_file_list(const nlohmann::json& value) {
    std::vector<FileActivity> out;
    if (value.is_null()) return out;
    std::vector<nlohmann::json> items = value.is_array()
        ? std::vector<nlohmann::json>(value.begin(), value.end())
        : std::vector<nlohmann::json>{value};
    for (const auto& item : items) {
        if (item.is_object()) {
            FileActivity fa;
            if (item.contains("path")) fa.path = stringify_lenient(item["path"]);
            if (item.contains("summary")) fa.summary = stringify_lenient(item["summary"]);
            if (item.contains("key_code") && !item["key_code"].is_null()) {
                fa.key_code = stringify_lenient(item["key_code"]);
            }
            out.push_back(std::move(fa));
        } else {
            std::string path = stringify_lenient(item);
            if (!path.empty()) {
                out.push_back(FileActivity{std::move(path), "", std::nullopt});
            }
        }
    }
    return out;
}

std::optional<std::string> lenient_string_opt(const nlohmann::json& value) {
    if (value.is_null()) return std::nullopt;
    return stringify_lenient(value);
}

bool blank(const std::string& s) {
    return s.find_first_not_of(" \t\r\n") == std::string::npos;
}

void normalize(StructuredSummary& summary) {
    for (auto* list : {&summary.user_intent, &summary.technical_concepts,
                       &summary.errors_and_fixes, &summary.problem_solving,
                       &summary.user_messages, &summary.pending_tasks}) {
        list->erase(std::remove_if(list->begin(), list->end(), blank), list->end());
    }
    for (auto& file : summary.files) {
        if (file.key_code && blank(*file.key_code)) file.key_code = std::nullopt;
    }
    summary.files.erase(
        std::remove_if(summary.files.begin(), summary.files.end(),
                       [](const FileActivity& f) {
                           return blank(f.path) && blank(f.summary) && !f.key_code;
                       }),
        summary.files.end());
    if (summary.current_work && blank(*summary.current_work)) summary.current_work = std::nullopt;
    if (summary.next_step && blank(*summary.next_step)) summary.next_step = std::nullopt;
}

bool is_empty(const StructuredSummary& summary) {
    return summary.user_intent.empty() && summary.technical_concepts.empty()
        && summary.files.empty() && summary.errors_and_fixes.empty()
        && summary.problem_solving.empty() && summary.user_messages.empty()
        && summary.pending_tasks.empty() && !summary.current_work && !summary.next_step;
}

StructuredSummary summary_from_json(const nlohmann::json& j) {
    StructuredSummary s;
    if (j.contains("user_intent")) s.user_intent = lenient_string_list(j["user_intent"]);
    if (j.contains("technical_concepts")) s.technical_concepts = lenient_string_list(j["technical_concepts"]);
    if (j.contains("files")) s.files = lenient_file_list(j["files"]);
    if (j.contains("errors_and_fixes")) s.errors_and_fixes = lenient_string_list(j["errors_and_fixes"]);
    if (j.contains("problem_solving")) s.problem_solving = lenient_string_list(j["problem_solving"]);
    if (j.contains("user_messages")) s.user_messages = lenient_string_list(j["user_messages"]);
    if (j.contains("pending_tasks")) s.pending_tasks = lenient_string_list(j["pending_tasks"]);
    if (j.contains("current_work")) s.current_work = lenient_string_opt(j["current_work"]);
    if (j.contains("next_step")) s.next_step = lenient_string_opt(j["next_step"]);
    return s;
}

// ============ JSON document extraction ============

constexpr const char* kTerminator = "</analysis>";

/// A brace-balanced object anchored at (after whitespace) the start of `text`,
/// or nullopt. Brace-balancing rather than fence-delimiting because string
/// values may legally contain fences.
std::optional<std::string> leading_object(const std::string& text) {
    size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos || text[start] != '{') return std::nullopt;

    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = start; i < text.size(); ++i) {
        char ch = text[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '"') {
            in_string = true;
        } else if (ch == '{') {
            depth++;
        } else if (ch == '}') {
            depth--;
            if (depth == 0) return text.substr(start, i - start + 1);
        }
    }
    return std::nullopt;
}

size_t count_occurrences(const std::string& text, const char* needle) {
    size_t count = 0;
    size_t pos = 0;
    size_t needle_len = strlen(needle);
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        count++;
        pos += needle_len;
    }
    return count;
}

/// Candidate JSON documents in the response, tried in order until one parses:
/// after each `</analysis>` terminator (last first) the post-terminator ```json
/// fences (last first) then a leading object, and finally a leading object of
/// the whole text.
std::vector<std::string> json_candidates(const std::string& text) {
    const size_t term_len = strlen(kTerminator);

    std::vector<size_t> cuts;
    size_t pos = 0;
    while ((pos = text.find(kTerminator, pos)) != std::string::npos) {
        cuts.push_back(pos + term_len);
        pos += term_len;
    }
    if (cuts.empty()) cuts.push_back(0);

    std::vector<std::string> candidates;
    for (auto it = cuts.rbegin(); it != cuts.rend(); ++it) {
        std::string tail = text.substr(*it);
        size_t later_terminators = count_occurrences(tail, kTerminator);

        std::vector<size_t> fence_positions;
        pos = 0;
        while ((pos = tail.find("```json", pos)) != std::string::npos) {
            fence_positions.push_back(pos);
            pos += 7;
        }
        for (auto fp = fence_positions.rbegin(); fp != fence_positions.rend(); ++fp) {
            auto obj = leading_object(tail.substr(*fp + 7));
            if (obj && count_occurrences(*obj, kTerminator) == later_terminators) {
                candidates.push_back(*obj);
            }
        }
        auto obj = leading_object(tail);
        if (obj && count_occurrences(*obj, kTerminator) == later_terminators) {
            candidates.push_back(*obj);
        }
    }
    auto whole = leading_object(text);
    if (whole) candidates.push_back(*whole);

    std::vector<std::string> deduped;
    for (const auto& c : candidates) {
        if (std::find(deduped.begin(), deduped.end(), c) == deduped.end()) {
            deduped.push_back(c);
        }
    }
    return deduped;
}

// ============ tool response helpers ============

bool has_tool_response(const Message& msg) {
    for (const auto& block : msg.content) {
        if (std::holds_alternative<ToolResponse>(block)) return true;
    }
    return false;
}

std::string tool_result_text(const CallToolResult& result) {
    std::vector<std::string> items;
    for (const auto& c : result.content) {
        if (c.is_object() && c.value("type", "") == "text" && c.contains("text")) {
            if (c["text"].is_string()) items.push_back(c["text"].get_ref<const std::string&>());
        }
    }
    std::string joined;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) joined += "\n";
        joined += items[i];
    }
    return joined;
}

// ============ compaction internals ============

constexpr int kRemovalPercentages[] = {0, 10, 20, 50, 100};

constexpr const char* kConversationContinuationText =
    "Your context was compacted. The previous message contains a summary of the conversation so far.\n"
    "Do not mention that you read a summary or that conversation summarization occurred.\n"
    "Just continue the conversation naturally based on the summarized context.";

constexpr const char* kToolLoopContinuationText =
    "Your context was compacted. The previous message contains a summary of the conversation so far.\n"
    "Do not mention that you read a summary or that conversation summarization occurred.\n"
    "Continue calling tools as necessary to complete the task.";

constexpr const char* kManualCompactContinuationText =
    "Your context was compacted at the user's request. The previous message contains a summary of the conversation so far.\n"
    "Do not mention that you read a summary or that conversation summarization occurred.\n"
    "Just continue the conversation naturally based on the summarized context.";

/// Messages with the middle-out share of tool responses removed.
std::vector<const Message*> filter_tool_responses(const std::vector<Message>& messages, int remove_percent) {
    if (remove_percent == 0) {
        std::vector<const Message*> all;
        all.reserve(messages.size());
        for (const auto& m : messages) all.push_back(&m);
        return all;
    }

    std::vector<size_t> tool_indices;
    for (size_t i = 0; i < messages.size(); ++i) {
        if (has_tool_response(messages[i])) tool_indices.push_back(i);
    }
    if (tool_indices.empty()) {
        std::vector<const Message*> all;
        all.reserve(messages.size());
        for (const auto& m : messages) all.push_back(&m);
        return all;
    }

    size_t num_to_remove = std::min<size_t>(
        tool_indices.size(),
        std::max<size_t>(1, (tool_indices.size() * static_cast<size_t>(remove_percent)) / 100));
    size_t middle = tool_indices.size() / 2;

    // Middle-out removal order: the middle element first, then expanding
    // outward, alternating sides.
    std::vector<size_t> order;
    order.reserve(tool_indices.size());
    order.push_back(middle);
    size_t left = middle;
    size_t right = middle + 1;
    while (order.size() < tool_indices.size()) {
        if (left > 0) order.push_back(--left);
        if (order.size() < tool_indices.size() && right < tool_indices.size()) {
            order.push_back(right++);
        }
    }

    std::vector<size_t> indices_to_remove;
    indices_to_remove.reserve(num_to_remove);
    for (size_t i = 0; i < num_to_remove; ++i) {
        indices_to_remove.push_back(tool_indices[order[i]]);
    }

    std::vector<const Message*> out;
    for (size_t i = 0; i < messages.size(); ++i) {
        if (std::find(indices_to_remove.begin(), indices_to_remove.end(), i) == indices_to_remove.end()) {
            out.push_back(&messages[i]);
        }
    }
    return out;
}

void apply_structured_summary(Message& response) {
    auto summary = parse_structured_summary(response.as_concat_text());
    if (!summary) return;
    auto rendered = render_structured_summary(*summary);
    if (rendered && !rendered->empty() &&
        rendered->find_first_not_of(" \t\r\n") != std::string::npos) {
        response.content = {TextContent{*rendered}};
    }
}

} // namespace

// ============ StructuredSummary ============

std::optional<StructuredSummary> parse_structured_summary(const std::string& response_text) {
    for (const auto& candidate : json_candidates(response_text)) {
        auto parsed = nlohmann::json::parse(candidate, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) continue;
        StructuredSummary summary = summary_from_json(parsed);
        normalize(summary);
        if (!is_empty(summary)) return summary;
    }
    return std::nullopt;
}

Result<std::string> render_structured_summary(const StructuredSummary& summary) {
    nlohmann::json context;
    context["user_intent"] = summary.user_intent;
    context["technical_concepts"] = summary.technical_concepts;
    context["errors_and_fixes"] = summary.errors_and_fixes;
    context["problem_solving"] = summary.problem_solving;
    context["user_messages"] = summary.user_messages;
    context["pending_tasks"] = summary.pending_tasks;
    context["current_work"] = summary.current_work ? nlohmann::json(*summary.current_work)
                                                   : nlohmann::json(nullptr);
    context["next_step"] = summary.next_step ? nlohmann::json(*summary.next_step)
                                             : nlohmann::json(nullptr);
    nlohmann::json files = nlohmann::json::array();
    for (const auto& file : summary.files) {
        files.push_back({
            {"path", file.path},
            {"summary", file.summary},
            {"key_code", file.key_code ? nlohmann::json(*file.key_code) : nlohmann::json(nullptr)},
        });
    }
    context["files"] = std::move(files);

    try {
        return Result<std::string>::ok(
            PromptTemplate::global().render("compaction_summary.md", context));
    } catch (const std::exception& e) {
        return Result<std::string>::err(
            make_error(ErrorCode::SerializeError, std::string("failed to render summary: ") + e.what()));
    }
}

// ============ message formatting and token estimation ============

std::string format_message_for_compacting(const Message& msg) {
    std::vector<std::string> parts;
    for (const auto& block : msg.content) {
        if (const auto* text = std::get_if<TextContent>(&block)) {
            parts.push_back(text->text);
        } else if (const auto* image = std::get_if<ImageContent>(&block)) {
            parts.push_back("[image: " + image->mime_type + "]");
        } else if (const auto* req = std::get_if<ToolRequest>(&block)) {
            if (std::holds_alternative<CallToolRequestParams>(req->tool_call)) {
                const auto& params = std::get<CallToolRequestParams>(req->tool_call);
                parts.push_back("tool_request(" + params.name + "): " + params.arguments.dump());
            } else {
                parts.push_back("tool_request: [error]");
            }
        } else if (const auto* res = std::get_if<ToolResponse>(&block)) {
            if (std::holds_alternative<CallToolResult>(res->tool_result)) {
                const auto& result = std::get<CallToolResult>(res->tool_result);
                std::string text = tool_result_text(result);
                if (text.empty()) {
                    parts.push_back("tool_response: [non-text content]");
                } else {
                    parts.push_back("tool_response: " + text);
                }
            } else {
                parts.push_back("tool_response: [error]");
            }
        } else if (const auto* confirm = std::get_if<ToolConfirmationRequest>(&block)) {
            parts.push_back("tool_confirmation_request: " + confirm->tool_name);
        } else if (const auto* note = std::get_if<SystemNotificationContent>(&block)) {
            parts.push_back("system_notification: " + note->msg);
        }
    }

    std::string content;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) content += "\n";
        content += parts[i];
    }
    if (content.empty()) {
        return std::string("[") + to_string(msg.role) + "]: <empty message>";
    }
    return std::string("[") + to_string(msg.role) + "]: " + content;
}

int64_t estimate_tokens(const Message& msg) {
    int64_t bytes = 0;
    for (const auto& block : msg.content) {
        if (const auto* text = std::get_if<TextContent>(&block)) {
            bytes += static_cast<int64_t>(text->text.size());
        } else if (std::holds_alternative<ImageContent>(block)) {
            bytes += 64;
        } else if (const auto* thinking = std::get_if<ThinkingContent>(&block)) {
            bytes += static_cast<int64_t>(thinking->thinking.size());
        } else if (const auto* req = std::get_if<ToolRequest>(&block)) {
            if (std::holds_alternative<CallToolRequestParams>(req->tool_call)) {
                const auto& params = std::get<CallToolRequestParams>(req->tool_call);
                bytes += static_cast<int64_t>(params.name.size() + params.arguments.dump().size());
            }
        } else if (const auto* res = std::get_if<ToolResponse>(&block)) {
            if (std::holds_alternative<CallToolResult>(res->tool_result)) {
                bytes += static_cast<int64_t>(
                    tool_result_text(std::get<CallToolResult>(res->tool_result)).size());
            }
        }
    }
    return std::max<int64_t>(1, bytes / 3);
}

int64_t estimate_conversation_tokens(const Conversation& conv) {
    int64_t total = 0;
    for (const auto& msg : conv.messages()) {
        total += estimate_tokens(msg);
    }
    return total;
}

bool check_if_compaction_needed(const Conversation& conv, double threshold, int64_t context_limit) {
    if (threshold <= 0.0 || threshold >= 1.0) return false;
    if (context_limit <= 0) return false;
    double ratio = static_cast<double>(estimate_conversation_tokens(conv)) /
                   static_cast<double>(context_limit);
    return ratio > threshold;
}

// ============ compact_messages ============

Result<CompactionResult> compact_messages(
    const Conversation& conversation,
    Provider& provider,
    const ModelConfig& model_config,
    bool manual_compact) {

    const auto& messages = conversation.messages();

    // Preserve the most recent text-only user message (non-manual compacts).
    std::optional<Message> preserved_user_message;
    bool is_most_recent = false;
    if (!manual_compact) {
        for (size_t i = messages.size(); i-- > 0;) {
            const auto& msg = messages[i];
            if (msg.role != Role::User) continue;
            bool has_text = false;
            bool has_tool = false;
            for (const auto& block : msg.content) {
                if (std::holds_alternative<TextContent>(block)) has_text = true;
                if (std::holds_alternative<ToolRequest>(block) ||
                    std::holds_alternative<ToolResponse>(block)) {
                    has_tool = true;
                }
            }
            if (has_text && !has_tool) {
                preserved_user_message = msg;
                is_most_recent = (i == messages.size() - 1);
                break;
            }
        }
    }

    // Try progressively removing more tool responses from the middle so the
    // summarization call itself fits in the context window.
    const char* kSummarizeRequest =
        "Please summarize the conversation history provided in the system prompt.";
    for (size_t attempt = 0; attempt < std::size(kRemovalPercentages); ++attempt) {
        auto filtered = filter_tool_responses(messages, kRemovalPercentages[attempt]);

        std::string messages_text;
        for (size_t i = 0; i < filtered.size(); ++i) {
            if (i) messages_text += "\n";
            messages_text += format_message_for_compacting(*filtered[i]);
        }

        std::string system_prompt;
        try {
            system_prompt = PromptTemplate::global().render(
                "compaction.md", {{"messages", messages_text}});
        } catch (const std::exception& e) {
            return Result<CompactionResult>::err(make_error(
                ErrorCode::SerializeError, std::string("failed to render compaction prompt: ") + e.what()));
        }

        Message user_message = Message::user().with_text(kSummarizeRequest);
        auto complete_result = provider.complete(model_config, system_prompt, {user_message}, {});
        if (complete_result) {
            Message response = std::move(*complete_result);
            response.role = Role::User;
            apply_structured_summary(response);

            std::vector<Message> final_messages;
            final_messages.push_back(std::move(response));

            const char* continuation_text;
            if (manual_compact) {
                continuation_text = kManualCompactContinuationText;
            } else if (is_most_recent) {
                continuation_text = kConversationContinuationText;
            } else {
                continuation_text = kToolLoopContinuationText;
            }
            final_messages.push_back(Message::assistant().with_text(continuation_text));

            if (preserved_user_message) {
                final_messages.push_back(std::move(*preserved_user_message));
            }

            return Result<CompactionResult>::ok(
                CompactionResult{Conversation(std::move(final_messages))});
        }

        const auto& error = complete_result.error();
        if (error.code == ErrorCode::ContextLengthError) {
            if (attempt + 1 < std::size(kRemovalPercentages)) {
                continue;
            }
            return Result<CompactionResult>::err(make_error(
                ErrorCode::ProviderError,
                "Failed to compact: context limit exceeded even after removing all tool responses"));
        }
        return Result<CompactionResult>::err(error);
    }

    return Result<CompactionResult>::err(make_error(
        ErrorCode::ProviderError,
        "Failed to compact: context limit exceeded even after removing all tool responses"));
}

} // namespace goose
