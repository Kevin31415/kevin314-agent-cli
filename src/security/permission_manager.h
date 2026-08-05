#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../config/paths.h"
#include "../core/tool.h"

namespace goose {

enum class PermissionLevel { AlwaysAllow, AskBefore, NeverAllow };

const char* to_string(PermissionLevel level);

struct PermissionConfig {
    std::vector<std::string> always_allow;
    std::vector<std::string> ask_before;
    std::vector<std::string> never_allow;
};

// Persistent permission configuration stored in permission.yaml under the
// config directory. Two categories: "user" (explicit user preferences) and
// "smart_approve" (derived from LLM read-only detection / tool annotations).
class PermissionManager {
public:
    explicit PermissionManager(std::filesystem::path config_dir = paths::config_dir());

    std::optional<PermissionLevel> get_user_permission(const std::string& tool_name) const;
    std::optional<PermissionLevel> get_smart_approve_permission(const std::string& tool_name) const;

    void update_user_permission(const std::string& tool_name, PermissionLevel level);
    void update_smart_approve_permission(const std::string& tool_name, PermissionLevel level);

    // Tools explicitly annotated as non-read-only are cached globally as
    // AskBefore under smart_approve (a negative, name-wide decision).
    void apply_tool_annotations(const std::vector<Tool>& tools);

    void remove_extension(const std::string& extension_name);

    std::vector<std::string> get_permission_names() const;
    const std::filesystem::path& config_path() const { return config_path_; }

private:
    std::optional<PermissionLevel> get_permission(const std::string& category,
                                                  const std::string& tool_name) const;
    void update_permission(const std::string& category,
                           const std::string& tool_name, PermissionLevel level);
    void save() const;

    std::filesystem::path config_path_;
    std::unordered_map<std::string, PermissionConfig> permission_map_;
};

} // namespace goose
