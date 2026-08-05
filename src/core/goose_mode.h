#pragma once

#include <string>
#include <optional>

namespace goose {

enum class GooseMode {
    Auto,
    Approve,
    SmartApprove,
    Chat,
    BypassPermissions
};

inline const char* to_string(GooseMode mode) {
    switch (mode) {
        case GooseMode::Auto: return "auto";
        case GooseMode::Approve: return "approve";
        case GooseMode::SmartApprove: return "smart_approve";
        case GooseMode::Chat: return "chat";
        case GooseMode::BypassPermissions: return "bypass_permissions";
    }
#ifdef __GNUC__
    __builtin_unreachable();
#elif defined(_MSC_VER)
    __assume(false);
#endif
}

inline std::optional<GooseMode> goose_mode_from_string(const std::string& s) {
    if (s == "auto") return GooseMode::Auto;
    if (s == "approve") return GooseMode::Approve;
    if (s == "smart_approve" || s == "smart-approve") return GooseMode::SmartApprove;
    if (s == "chat") return GooseMode::Chat;
    if (s == "bypass_permissions" || s == "bypass-permissions") return GooseMode::BypassPermissions;
    return std::nullopt;
}

} // namespace goose
