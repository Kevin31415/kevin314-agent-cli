#pragma once

#include <string>
#include <optional>
#include <unordered_map>
#include <filesystem>
#include <mutex>
#include <yaml-cpp/yaml.h>
#include "../utils/error.h"
#include "../core/goose_mode.h"
#include "../extension/extension_config.h"

namespace goose {

struct ProviderEntry {
    bool enabled = true;
    std::string type = "openai";
    std::string model;
    std::string base_url;
    bool configured = false;
};

class Config {
public:
    static Config& global();

    std::optional<std::string> get_goose_provider() const;
    std::optional<std::string> get_goose_model() const;
    GooseMode get_goose_mode() const;
    void set_goose_mode(GooseMode mode);

    Result<std::string> get_param(const std::string& key) const;
    void set_param(const std::string& key, const std::string& value);

    // 思考程度（low|medium|high），来自 GOOSE_REASONING_EFFORT / KACLI_REASONING_EFFORT / REASONING_EFFORT。
    std::string get_reasoning_effort() const;

    // 系统提示词方案（system.md | tiny_model_system.md）。
    std::string get_system_prompt_template() const;
    void set_system_prompt_template(const std::string& name);

    std::optional<ProviderEntry> get_provider(const std::string& name) const;
    std::vector<std::string> get_configured_providers() const;
    std::string get_active_provider() const;
    std::string get_active_model() const;
    std::vector<ExtensionConfig> get_extensions() const;

private:
    Config() = default;
    void load();
    void load_file(const std::filesystem::path& path);
    void merge_node(YAML::Node& base, const YAML::Node& overlay);
    void save();

    YAML::Node config_;
    mutable std::once_flag load_flag_;
};

} // namespace goose
