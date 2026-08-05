#pragma once

#include <string>
#include <optional>
#include <vector>
#include <nlohmann/json.hpp>

namespace goose {

struct Tool {
    std::string name;
    std::string description;
    nlohmann::json input_schema;
    // Explicit write annotation (false = modifies state, true = read-only).
    // Absent = unknown; SmartApprove falls back to LLM detection.
    std::optional<bool> read_only_hint;
};

void to_json(nlohmann::json& j, const Tool& t);
void from_json(const nlohmann::json& j, Tool& t);

} // namespace goose
