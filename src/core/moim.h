#pragma once

#include <cstdint>
#include <string>

#include "types.h"

namespace goose {

constexpr int64_t kMinContextForMoim = 32'000;

inline constexpr const char* kTurnContextTag = "turn-context";
inline constexpr const char* kCurrentTimeTag = "current-time";
inline constexpr const char* kWorkingDirectoryTag = "working-directory";

/// The `# Turn Context` explanation block appended to the system prompt.
std::string system_prompt_block();

/// Compose the `<turn-context>` block. `compaction` is shown only when at
/// least half of the budgeted context is used; `turn-budget` only when at
/// least half of the turn budget is spent.
std::string compose_moim(
    const std::string& working_dir,
    int64_t total_tokens,
    int64_t context_limit,
    double compaction_threshold,
    uint32_t turns_taken,
    uint32_t max_turns);

/// Inject the turn-context block at the start of the most recent text-only
/// user message. Returns the conversation unchanged when the context window
/// is too small or no suitable user message exists. The injection is transient
/// - it is never persisted to the session history.
Conversation inject_moim(
    Conversation conversation,
    const std::string& working_dir,
    int64_t context_limit,
    double compaction_threshold,
    uint32_t turns_taken,
    uint32_t max_turns,
    int64_t total_tokens);

} // namespace goose
