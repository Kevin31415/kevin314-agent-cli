#pragma once

#include <string>
#include <vector>
#include "tool.h"
#include "goose_mode.h"

namespace goose {

class PromptManager {
public:
    std::string get_system_prompt(const std::vector<Tool>& tools,
                                   GooseMode mode = GooseMode::Auto,
                                   const std::string& working_dir = "",
                                   const std::optional<std::string>& additional_prompt = std::nullopt) const;

private:
    std::string base_prompt_;
};

} // namespace goose
