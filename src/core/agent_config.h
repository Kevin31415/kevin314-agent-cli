#pragma once

#include "../config/config.h"
#include "../session/session_manager.h"

namespace goose {

struct AgentConfig {
    SessionManager* session_manager = nullptr;
    bool disable_session_naming = false;
    std::string working_dir;
    std::optional<std::string> additional_system_prompt;
};

} // namespace goose
