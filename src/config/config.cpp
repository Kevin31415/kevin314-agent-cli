#include "config.h"
#include "paths.h"
#include "../utils/format.h"
#include <spdlog/spdlog.h>
#include <cstdlib>
#include <fstream>

namespace goose {

static const char* goose_env(const char* name) {
    std::string goose_key = std::string("GOOSE_") + name;
    if (const char* v = std::getenv(goose_key.c_str())) return v;
    std::string kacli_key = std::string("KACLI_") + name;
    return std::getenv(kacli_key.c_str());
}

Config& Config::global() {
    static Config config;
    return config;
}

void Config::load() {
    config_ = YAML::Node(YAML::NodeType::Map);

    auto system_config = paths::config_dir() / "config.yaml";
    load_file(system_config);

    const char* additional = goose_env("ADDITIONAL_CONFIG_FILES");
    if (additional) {
        std::string files(additional);
#ifdef _WIN32
        const char sep = ';';
#else
        const char sep = ':';
#endif
        size_t pos = 0;
        while ((pos = files.find(sep)) != std::string::npos) {
            load_file(std::filesystem::path(files.substr(0, pos)));
            files = files.substr(pos + 1);
        }
        if (!files.empty()) {
            load_file(std::filesystem::path(files));
        }
    }

    load_file(paths::config_file());

    spdlog::info("Config loaded from: {}", paths::config_file().string());
}

void Config::load_file(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        spdlog::debug("Config file not found: {}", path.string());
        return;
    }

    try {
        auto node = YAML::LoadFile(path.string());
        merge_node(config_, node);
        spdlog::debug("Merged config from: {}", path.string());
    } catch (const YAML::Exception& e) {
        spdlog::warn("Failed to parse config file {}: {}", path.string(), e.what());
    }
}

void Config::merge_node(YAML::Node& base, const YAML::Node& overlay) {
    if (!overlay.IsMap()) {
        base = overlay;
        return;
    }

    for (auto it = overlay.begin(); it != overlay.end(); ++it) {
        std::string key = it->first.as<std::string>();
        const YAML::Node& overlay_val = it->second;

        YAML::Node base_val = base[key];
        if (base_val && base_val.IsMap() && overlay_val.IsMap()) {
            merge_node(base_val, overlay_val);
        } else {
            base[key] = overlay_val;
        }
    }
}

std::optional<std::string> Config::get_goose_provider() const {
    const char* v = goose_env("PROVIDER");
    if (v) return std::string(v);

    std::call_once(load_flag_, [this]() { const_cast<Config*>(this)->load(); });

    if (config_["active_provider"]) {
        return config_["active_provider"].as<std::string>();
    }
    return std::nullopt;
}

std::optional<std::string> Config::get_goose_model() const {
    const char* v = goose_env("MODEL");
    if (v) return std::string(v);

    std::call_once(load_flag_, [this]() { const_cast<Config*>(this)->load(); });

    auto provider = get_active_provider();
    if (config_["providers"] && config_["providers"][provider]) {
        auto& p = config_["providers"][provider];
        if (p["model"]) {
            return p["model"].as<std::string>();
        }
    }
    return std::nullopt;
}

GooseMode Config::get_goose_mode() const {
    const char* v = goose_env("MODE");
    if (v) {
        auto parsed = goose_mode_from_string(v);
        if (parsed) return *parsed;
        spdlog::warn("未知 GOOSE_MODE '{}'，回退到 approve（要求批准）", v);
        return GooseMode::Approve;
    }

    std::call_once(load_flag_, [this]() { const_cast<Config*>(this)->load(); });

    auto result = get_param("GOOSE_MODE");
    if (!result) result = get_param("KACLI_MODE");
    if (result) {
        auto parsed = goose_mode_from_string(*result);
        if (parsed) return *parsed;
        spdlog::warn("未知 GOOSE_MODE '{}'，回退到 approve（要求批准）", *result);
        return GooseMode::Approve;
    }
    return GooseMode::Auto;
}

void Config::set_goose_mode(GooseMode mode) {
    set_param("GOOSE_MODE", to_string(mode));
}

Result<std::string> Config::get_param(const std::string& key) const {
    const char* v = std::getenv(key.c_str());
    if (v) return Result<std::string>::ok(std::string(v));

    std::call_once(load_flag_, [this]() { const_cast<Config*>(this)->load(); });

    if (config_[key]) {
        try {
            return Result<std::string>::ok(config_[key].as<std::string>());
        } catch (...) {}
    }

    return Result<std::string>::err(make_error(ErrorCode::ConfigError, "Not found: " + key));
}

std::string Config::get_reasoning_effort() const {
    for (const char* key : {"GOOSE_REASONING_EFFORT", "KACLI_REASONING_EFFORT", "REASONING_EFFORT"}) {
        auto result = get_param(key);
        if (result) return *result;
    }
    return "";
}

std::string Config::get_system_prompt_template() const {
    for (const char* key : {"KACLI_SYSTEM_PROMPT_TEMPLATE", "SYSTEM_PROMPT_TEMPLATE"}) {
        auto result = get_param(key);
        if (result) return *result;
    }
    return "system.md";
}

void Config::set_system_prompt_template(const std::string& name) {
    set_param("KACLI_SYSTEM_PROMPT_TEMPLATE", name);
}

void Config::set_param(const std::string& key, const std::string& value) {
    std::call_once(load_flag_, [this]() { load(); });

    config_[key] = value;

    save();
}

void Config::save() {
    auto path = paths::config_file();
    try {
        std::filesystem::create_directories(path.parent_path());

        // Write to a temp file and rename so a crash mid-write never leaves a
        // truncated config behind.
        auto tmp_path = path;
        tmp_path += ".tmp";

        std::ofstream ofs(tmp_path);
        if (!ofs.is_open()) {
            spdlog::error("Failed to open config file for writing: {}", tmp_path.string());
            return;
        }
        ofs << config_;
        if (ofs.fail()) {
            spdlog::error("Failed to write config file: {}", tmp_path.string());
            ofs.close();
            std::filesystem::remove(tmp_path);
            return;
        }
        ofs.close();
        utils::set_file_permissions_private(tmp_path.string());

        std::error_code ec;
        std::filesystem::rename(tmp_path, path, ec);
        if (ec) {
            // Fall back to a direct write when rename is unavailable.
            spdlog::warn("Atomic config rename failed ({}); writing directly", ec.message());
            std::ofstream direct(path);
            direct << config_;
            std::filesystem::remove(tmp_path);
            return;
        }
        spdlog::info("Config saved to: {}", path.string());
    } catch (const std::exception& e) {
        spdlog::error("Failed to save config: {}", e.what());
    }
}

std::optional<ProviderEntry> Config::get_provider(const std::string& name) const {
    std::call_once(load_flag_, [this]() { const_cast<Config*>(this)->load(); });

    if (config_["providers"] && config_["providers"][name]) {
        auto& p = config_["providers"][name];
        ProviderEntry entry;
        entry.enabled = p["enabled"] ? p["enabled"].as<bool>() : true;
        entry.type = p["type"] ? p["type"].as<std::string>() : "openai";
        entry.model = p["model"] ? p["model"].as<std::string>() : "";
        entry.base_url = p["base_url"] ? p["base_url"].as<std::string>() : "";
        entry.configured = p["configured"] ? p["configured"].as<bool>() : false;
        return entry;
    }
    return std::nullopt;
}

std::vector<std::string> Config::get_configured_providers() const {
    std::vector<std::string> result;
    std::call_once(load_flag_, [this]() { const_cast<Config*>(this)->load(); });
    if (config_["providers"]) {
        for (auto it = config_["providers"].begin(); it != config_["providers"].end(); ++it) {
            result.push_back(it->first.as<std::string>());
        }
    }
    return result;
}

std::string Config::get_active_provider() const {
    const char* v = goose_env("PROVIDER");
    if (v) return std::string(v);

    std::call_once(load_flag_, [this]() { const_cast<Config*>(this)->load(); });

    if (config_["active_provider"]) {
        return config_["active_provider"].as<std::string>();
    }
    return "openai";
}

std::string Config::get_active_model() const {
    const char* v = goose_env("MODEL");
    if (v) return std::string(v);

    auto provider = get_active_provider();
    auto entry = get_provider(provider);
    if (entry && !entry->model.empty()) {
        return entry->model;
    }
    return "gpt-4o";
}

std::vector<ExtensionConfig> Config::get_extensions() const {
    std::vector<ExtensionConfig> result;

    std::call_once(load_flag_, [this]() { const_cast<Config*>(this)->load(); });

    if (!config_["extensions"]) return result;

    for (auto it = config_["extensions"].begin(); it != config_["extensions"].end(); ++it) {
        std::string key = it->first.as<std::string>();
        auto& ext = it->second;

        if (ext["enabled"] && !ext["enabled"].as<bool>()) continue;

        std::string type_str = ext["type"] ? ext["type"].as<std::string>() : "builtin";
        std::string name = ext["name"] ? ext["name"].as<std::string>() : key;
        std::string desc = ext["description"] ? ext["description"].as<std::string>() : "";
        uint64_t timeout = ext["timeout"] ? ext["timeout"].as<uint64_t>() : 300;

        if (type_str == "builtin") {
            result.push_back(ExtensionConfig::builtin(name));
        } else if (type_str == "stdio") {
            std::string cmd = ext["cmd"] ? ext["cmd"].as<std::string>() : "";
            std::vector<std::string> args;
            if (ext["args"]) {
                for (const auto& a : ext["args"]) {
                    args.push_back(a.as<std::string>());
                }
            }
            ExtensionConfig ec;
            ec.type = ExtensionConfig::Type::Stdio;
            ec.name = name;
            ec.description = desc;
            ec.cmd = cmd;
            ec.args = std::move(args);
            ec.timeout = timeout;
            result.push_back(std::move(ec));
        }
    }

    return result;
}

} // namespace goose
