#include "core/tool.h"
#include <nlohmann/json.hpp>

namespace goose {

void to_json(nlohmann::json& j, const Tool& t) {
    j = nlohmann::json{
        {"name", t.name},
        {"description", t.description},
        {"inputSchema", t.input_schema}
    };
}

void from_json(const nlohmann::json& j, Tool& t) {
    j.at("name").get_to(t.name);
    j.at("description").get_to(t.description);
    if (j.contains("inputSchema")) j.at("inputSchema").get_to(t.input_schema);
}

} // namespace goose
