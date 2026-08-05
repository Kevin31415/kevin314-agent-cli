#include "provider/model_config.h"

namespace goose {

void to_json(nlohmann::json& j, const ModelConfig& c) {
    j = nlohmann::json{
        {"modelName", c.model_name}
    };
    if (c.temperature) j["temperature"] = *c.temperature;
    if (c.max_tokens) j["maxTokens"] = *c.max_tokens;
    if (c.context_limit) j["contextLimit"] = *c.context_limit;
    if (!c.reasoning_effort.empty()) j["reasoningEffort"] = c.reasoning_effort;
}

void from_json(const nlohmann::json& j, ModelConfig& c) {
    j.at("modelName").get_to(c.model_name);
    if (j.contains("temperature")) c.temperature = j["temperature"].get<float>();
    if (j.contains("maxTokens")) c.max_tokens = j["maxTokens"].get<int32_t>();
    if (j.contains("contextLimit")) c.context_limit = j["contextLimit"].get<int64_t>();
    if (j.contains("reasoningEffort")) c.reasoning_effort = j["reasoningEffort"].get<std::string>();
}

} // namespace goose
