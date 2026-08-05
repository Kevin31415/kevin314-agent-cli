#pragma once

#include <string>
#include <vector>
#include <optional>

#include "../utils/error.h"
#include "../utils/prompt_template.h"
#include "../provider/base.h"
#include "types.h"

namespace goose {

constexpr double kDefaultCompactionThreshold = 0.8;
constexpr int64_t kDefaultContextLimit = 128000;

struct FileActivity {
    std::string path;
    std::string summary;
    std::optional<std::string> key_code;
};

/// Structured output of the compaction LLM call. Every list is ordered
/// most-important-first so the render template can cut from the tail. Fields
/// parse leniently (objects/numbers where strings were asked are stringified)
/// because models routinely enrich the schema.
struct StructuredSummary {
    std::vector<std::string> user_intent;
    std::vector<std::string> technical_concepts;
    std::vector<FileActivity> files;
    std::vector<std::string> errors_and_fixes;
    std::vector<std::string> problem_solving;
    std::vector<std::string> user_messages;
    std::vector<std::string> pending_tasks;
    std::optional<std::string> current_work;
    std::optional<std::string> next_step;
};

/// Extract the summary JSON document from the model's response. Returns
/// `nullopt` when no usable document is found so the caller keeps the raw
/// response text - the lossless fallback.
std::optional<StructuredSummary> parse_structured_summary(const std::string& response_text);

/// Render `compaction_summary.md` with the summary as template context.
Result<std::string> render_structured_summary(const StructuredSummary& summary);

/// Textual projection of one message for the compaction LLM input.
std::string format_message_for_compacting(const Message& msg);

/// Heuristic token estimate (no tiktoken in kacli): bytes / 3, floor 1.
int64_t estimate_tokens(const Message& msg);
int64_t estimate_conversation_tokens(const Conversation& conv);

/// True when the conversation's estimated tokens exceed
/// `threshold * context_limit`. Thresholds outside (0, 1) disable auto-compact.
bool check_if_compaction_needed(const Conversation& conv, double threshold, int64_t context_limit);

struct CompactionResult {
    Conversation conversation;
};

/// Summarize the conversation with the provider and replace its history with
/// the summary, a continuation hint, and (unless manual) the most recent
/// text-only user message.
Result<CompactionResult> compact_messages(
    const Conversation& conversation,
    Provider& provider,
    const ModelConfig& model_config,
    bool manual_compact);

} // namespace goose
