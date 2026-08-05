#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace goose {
namespace mcp {

struct ToolInfo {
    std::string name;
    std::string description;
    nlohmann::json input_schema;
};

} // namespace mcp
} // namespace goose
