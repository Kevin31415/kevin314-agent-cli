#include "provider/provider_usage.h"

namespace goose {

void to_json(nlohmann::json& j, const Usage& u) {
    j = nlohmann::json{};
    if (u.input_tokens) j["inputTokens"] = *u.input_tokens;
    if (u.output_tokens) j["outputTokens"] = *u.output_tokens;
    if (u.cache_read_input_tokens) j["cacheReadInputTokens"] = *u.cache_read_input_tokens;
    if (u.cache_write_input_tokens) j["cacheWriteInputTokens"] = *u.cache_write_input_tokens;
}

void from_json(const nlohmann::json& j, Usage& u) {
    if (j.contains("inputTokens")) u.input_tokens = j["inputTokens"].get<int64_t>();
    if (j.contains("outputTokens")) u.output_tokens = j["outputTokens"].get<int64_t>();
    if (j.contains("cacheReadInputTokens")) u.cache_read_input_tokens = j["cacheReadInputTokens"].get<int64_t>();
    if (j.contains("cacheWriteInputTokens")) u.cache_write_input_tokens = j["cacheWriteInputTokens"].get<int64_t>();
}

void to_json(nlohmann::json& j, const ProviderUsage& u) {
    j = nlohmann::json{
        {"provider", u.provider},
        {"usage", u.usage}
    };
}

void from_json(const nlohmann::json& j, ProviderUsage& u) {
    j.at("provider").get_to(u.provider);
    j.at("usage").get_to(u.usage);
}

} // namespace goose
