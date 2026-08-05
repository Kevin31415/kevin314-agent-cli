#include "security/permission_manager.h"

#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <fstream>

namespace goose {

namespace {
constexpr const char* kUserPermission = "user";
constexpr const char* kSmartApprovePermission = "smart_approve";
constexpr const char* kPermissionFile = "permission.yaml";
} // namespace

const char* to_string(PermissionLevel level) {
    switch (level) {
        case PermissionLevel::AlwaysAllow: return "always_allow";
        case PermissionLevel::AskBefore: return "ask_before";
        case PermissionLevel::NeverAllow: return "never_allow";
    }
    return "ask_before";
}

namespace {

std::vector<std::string> yaml_str_list(const YAML::Node& node) {
    std::vector<std::string> out;
    if (!node || !node.IsSequence()) return out;
    for (const auto& item : node) {
        out.push_back(item.as<std::string>());
    }
    return out;
}

} // namespace

PermissionManager::PermissionManager(std::filesystem::path config_dir) {
    config_path_ = std::move(config_dir) / kPermissionFile;

    if (!std::filesystem::exists(config_path_)) return;

    try {
        YAML::Node root = YAML::LoadFile(config_path_.string());
        for (const auto& category : {kUserPermission, kSmartApprovePermission}) {
            if (!root[category] || !root[category].IsMap()) continue;
            const auto& node = root[category];
            PermissionConfig config;
            config.always_allow = yaml_str_list(node["always_allow"]);
            config.ask_before = yaml_str_list(node["ask_before"]);
            config.never_allow = yaml_str_list(node["never_allow"]);
            permission_map_[category] = std::move(config);
        }
    } catch (const std::exception& e) {
        spdlog::error("Failed to parse permission config {}: {}. Starting with empty permissions.",
                      config_path_.string(), e.what());
    }
}

std::optional<PermissionLevel> PermissionManager::get_permission(
    const std::string& category, const std::string& tool_name) const {

    auto it = permission_map_.find(category);
    if (it == permission_map_.end()) return std::nullopt;

    const auto& config = it->second;
    if (std::find(config.always_allow.begin(), config.always_allow.end(), tool_name)
        != config.always_allow.end()) {
        return PermissionLevel::AlwaysAllow;
    }
    if (std::find(config.ask_before.begin(), config.ask_before.end(), tool_name)
        != config.ask_before.end()) {
        return PermissionLevel::AskBefore;
    }
    if (std::find(config.never_allow.begin(), config.never_allow.end(), tool_name)
        != config.never_allow.end()) {
        return PermissionLevel::NeverAllow;
    }
    return std::nullopt;
}

std::optional<PermissionLevel> PermissionManager::get_user_permission(
    const std::string& tool_name) const {
    return get_permission(kUserPermission, tool_name);
}

std::optional<PermissionLevel> PermissionManager::get_smart_approve_permission(
    const std::string& tool_name) const {
    return get_permission(kSmartApprovePermission, tool_name);
}

void PermissionManager::update_permission(const std::string& category,
                                          const std::string& tool_name,
                                          PermissionLevel level) {
    auto& config = permission_map_[category];
    config.always_allow.erase(
        std::remove(config.always_allow.begin(), config.always_allow.end(), tool_name),
        config.always_allow.end());
    config.ask_before.erase(
        std::remove(config.ask_before.begin(), config.ask_before.end(), tool_name),
        config.ask_before.end());
    config.never_allow.erase(
        std::remove(config.never_allow.begin(), config.never_allow.end(), tool_name),
        config.never_allow.end());

    switch (level) {
        case PermissionLevel::AlwaysAllow: config.always_allow.push_back(tool_name); break;
        case PermissionLevel::AskBefore: config.ask_before.push_back(tool_name); break;
        case PermissionLevel::NeverAllow: config.never_allow.push_back(tool_name); break;
    }
    save();
}

void PermissionManager::update_user_permission(const std::string& tool_name,
                                               PermissionLevel level) {
    update_permission(kUserPermission, tool_name, level);
}

void PermissionManager::update_smart_approve_permission(const std::string& tool_name,
                                                        PermissionLevel level) {
    update_permission(kSmartApprovePermission, tool_name, level);
}

void PermissionManager::apply_tool_annotations(const std::vector<Tool>& tools) {
    std::vector<std::string> write_annotated;
    for (const auto& tool : tools) {
        if (tool.read_only_hint == false) {
            write_annotated.push_back(tool.name);
        }
    }
    if (write_annotated.empty()) return;

    auto& config = permission_map_[kSmartApprovePermission];
    for (const auto& tool_name : write_annotated) {
        config.always_allow.erase(
            std::remove(config.always_allow.begin(), config.always_allow.end(), tool_name),
            config.always_allow.end());
        if (std::find(config.ask_before.begin(), config.ask_before.end(), tool_name)
            == config.ask_before.end()) {
            config.ask_before.push_back(tool_name);
        }
    }
    save();
}

void PermissionManager::remove_extension(const std::string& extension_name) {
    bool changed = false;
    for (auto& [category, config] : permission_map_) {
        auto strip = [&](std::vector<std::string>& list) {
            size_t before = list.size();
            list.erase(std::remove_if(list.begin(), list.end(),
                                      [&](const std::string& t) {
                                          return t.find(extension_name) == 0;
                                      }),
                       list.end());
            if (list.size() != before) changed = true;
        };
        strip(config.always_allow);
        strip(config.ask_before);
        strip(config.never_allow);
    }
    if (changed) save();
}

std::vector<std::string> PermissionManager::get_permission_names() const {
    std::vector<std::string> names;
    for (const auto& [category, config] : permission_map_) {
        names.push_back(category);
    }
    return names;
}

void PermissionManager::save() const {
    YAML::Node root(YAML::NodeType::Map);
    for (const auto& [category, config] : permission_map_) {
        if (config.always_allow.empty() && config.ask_before.empty() &&
            config.never_allow.empty()) {
            continue;
        }
        YAML::Node node(YAML::NodeType::Map);
        auto add_list = [&](const char* key, const std::vector<std::string>& list) {
            if (list.empty()) return;
            node[key] = YAML::Node(YAML::NodeType::Sequence);
            for (const auto& item : list) node[key].push_back(item);
        };
        add_list("always_allow", config.always_allow);
        add_list("ask_before", config.ask_before);
        add_list("never_allow", config.never_allow);
        root[category] = node;
    }

    try {
        std::filesystem::create_directories(config_path_.parent_path());
        std::ofstream ofs(config_path_);
        if (!ofs.is_open()) {
            spdlog::error("Failed to open permission config for writing: {}",
                          config_path_.string());
            return;
        }
        ofs << root;
    } catch (const std::exception& e) {
        spdlog::error("Failed to write permission config {}: {}",
                      config_path_.string(), e.what());
    }
}

} // namespace goose
