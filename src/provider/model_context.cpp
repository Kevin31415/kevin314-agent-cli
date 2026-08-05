#include "model_context.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "../config/config.h"
#include "../core/compaction.h"

namespace goose {

namespace {

struct ModelContextEntry {
    const char* prefix;
    int64_t limit;
};

constexpr ModelContextEntry kModelContexts[] = {
    {"gpt-oss", 128000},
    {"gpt-4o", 128000},
    {"gpt-4.1", 128000},
    {"gpt-4-turbo", 128000},
    {"gpt-4", 128000},
    {"gpt-3.5", 16385},
    {"o1", 200000},
    {"o3", 200000},
    {"o4", 200000},
    {"chatgpt", 128000},
    {"claude", 200000},
    {"deepseek", 64000},
    {"gemini", 1000000},
    {"llama", 128000},
    {"qwen", 128000},
    {"mistral", 32768},
    {"mixtral", 32768},
    {"codestral", 32768},
    {"command", 200000},
    {"grok", 131072},
    {"glm", 128000},
    {"kimi", 128000},
    {"moonshot", 128000},
    {"minimax", 128000},
    {"phi", 128000},
    {"internlm", 128000},
    {"yi", 128000},
    {"baichuan", 32768},
    {"nano", 128000},
    {"openchat", 8192},
    {"zephyr", 8192},
    {"text-embedding", 8192},
    {"text-moderation", 8192},
};

} // namespace

int64_t resolve_context_limit(const std::string& model_name) {
    if (auto env = Config::global().get_param("GOOSE_CONTEXT_LIMIT"); env) {
        try {
            int64_t n = std::stoll(env.value());
            if (n > 0) return n;
        } catch (const std::exception&) {
        }
    }

    std::string lower = model_name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto slash = lower.find('/');
    if (slash != std::string::npos) lower = lower.substr(slash + 1);

    for (const auto& entry : kModelContexts) {
        if (lower.rfind(entry.prefix, 0) == 0) {
            return entry.limit;
        }
    }
    return kDefaultContextLimit;
}

} // namespace goose
