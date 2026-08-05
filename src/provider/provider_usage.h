#pragma once

#include <string>
#include <optional>
#include <nlohmann/json.hpp>

namespace goose {

struct Usage {
    std::optional<int64_t> input_tokens;
    std::optional<int64_t> output_tokens;
    std::optional<int64_t> cache_read_input_tokens;
    std::optional<int64_t> cache_write_input_tokens;

    static Usage zero() {
        return Usage{
            .input_tokens = 0,
            .output_tokens = 0,
            .cache_read_input_tokens = std::nullopt,
            .cache_write_input_tokens = std::nullopt
        };
    }
};

struct ProviderUsage {
    std::string provider;
    Usage usage;
};

void to_json(nlohmann::json& j, const Usage& u);
void from_json(const nlohmann::json& j, Usage& u);
void to_json(nlohmann::json& j, const ProviderUsage& u);
void from_json(const nlohmann::json& j, ProviderUsage& u);

} // namespace goose
