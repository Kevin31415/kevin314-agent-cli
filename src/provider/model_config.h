#pragma once

#include <string>
#include <optional>
#include <nlohmann/json.hpp>

namespace goose {

struct ModelConfig {
    std::string model_name;
    std::optional<float> temperature;
    std::optional<int32_t> max_tokens;
    std::optional<int64_t> context_limit;
    std::string reasoning_effort;
};

void to_json(nlohmann::json& j, const ModelConfig& c);
void from_json(const nlohmann::json& j, ModelConfig& c);

} // namespace goose
