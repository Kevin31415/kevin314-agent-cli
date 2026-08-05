#include "extension/extension_manager.h"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace goose {

ExtensionManager::~ExtensionManager() {
    std::lock_guard<std::mutex> lock(mutex_);
    extensions_.clear();
}

std::string ExtensionManager::name_to_key(const std::string& name) {
    std::string key;
    key.reserve(name.size());
    for (char c : name) {
        if (std::isalnum(c)) {
            key += static_cast<char>(std::tolower(c));
        }
    }
    return key;
}

void ExtensionManager::add_builtin_developer() {
    auto key = name_to_key("developer");
    if (extensions_.count(key)) return;

    auto entry = std::make_shared<ExtensionEntry>();
    entry->config = ExtensionConfig::builtin("developer");
    entry->builtin = std::make_unique<DeveloperExtension>();
    entry->tools_cache = entry->builtin->list_tools();
    extensions_[key] = std::move(entry);
    spdlog::info("Added builtin extension: developer");
}

void ExtensionManager::add_extension(ExtensionConfig config) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (config.type == ExtensionConfig::Type::Builtin && name_to_key(config.name) == "developer") {
        add_builtin_developer();
        return;
    }

    auto key = name_to_key(config.name);

    if (extensions_.count(key)) {
        spdlog::info("Extension already loaded, skipping: {}", config.name);
        return;
    }

    if (config.type == ExtensionConfig::Type::Stdio) {
        auto client_result = McpClient::spawn(config.cmd, config.args, config.envs);
        if (!client_result) {
            spdlog::error("Failed to spawn MCP client for {}: {}",
                config.name, client_result.error().message);
            return;
        }

        auto init_result = client_result.value()->initialize();
        if (!init_result) {
            spdlog::error("Failed to initialize MCP client for {}: {}",
                config.name, init_result.error().message);
            return;
        }

        auto entry = std::make_shared<ExtensionEntry>();
        std::string saved_name = config.name;
        entry->config = std::move(config);
        entry->client = std::move(client_result.value());

        auto tools = entry->client->list_tools();
        if (tools) {
            for (const auto& ti : *tools) {
                Tool t;
                t.name = ti.name;
                t.description = ti.description;
                t.input_schema = ti.input_schema;
                entry->tools_cache.push_back(std::move(t));
            }
        }

        extensions_[key] = std::move(entry);
    spdlog::info("Added stdio extension: {} ({} tools)", saved_name,
        extensions_[key]->tools_cache.size());
    } else {
        spdlog::warn("Extension type not yet supported for {}: {}", config.name,
            static_cast<int>(config.type));
    }
}

std::vector<Tool> ExtensionManager::fetch_tools_for_entry(const ExtensionEntry& entry) const {
    std::vector<Tool> tools;

    bool unprefixed = (entry.config.type == ExtensionConfig::Type::Builtin ||
                       entry.config.type == ExtensionConfig::Type::Platform);

    for (const auto& tool : entry.tools_cache) {
        bool available = entry.config.available_tools.empty();
        if (!available) {
            for (const auto& at : entry.config.available_tools) {
                if (at == tool.name) {
                    available = true;
                    break;
                }
            }
        }
        if (!available) continue;

        Tool prefixed = tool;
        if (!unprefixed) {
            prefixed.name = entry.config.name + "__" + tool.name;
        }
        tools.push_back(std::move(prefixed));
    }

    return tools;
}

Result<std::vector<Tool>> ExtensionManager::list_tools() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Tool> all_tools;

    for (const auto& [key, entry] : extensions_) {
        auto tools = fetch_tools_for_entry(*entry);
        for (auto& tool : tools) {
            all_tools.push_back(std::move(tool));
        }
    }

    std::sort(all_tools.begin(), all_tools.end(),
        [](const Tool& a, const Tool& b) { return a.name < b.name; });

    return Result<std::vector<Tool>>::ok(std::move(all_tools));
}

Result<nlohmann::json> ExtensionManager::call_tool(
    const std::string& full_name,
    const nlohmann::json& arguments) {

    std::string ext_key;
    std::string tool_name;
    std::shared_ptr<ExtensionEntry> entry;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto dd_pos = full_name.find("__");
        if (dd_pos != std::string::npos) {
            // 优先用已注册扩展的 config.name 做最长前缀匹配；否则名称本身
            // 含 "__" 的扩展（如 my__ext）会被按第一个 __ 错误拆分。
            size_t best_len = 0;
            for (const auto& [key, ext_entry] : extensions_) {
                const std::string prefix = ext_entry->config.name + "__";
                if (full_name.size() > prefix.size() &&
                    full_name.compare(0, prefix.size(), prefix) == 0 &&
                    prefix.size() > best_len) {
                    best_len = prefix.size();
                    ext_key = key;
                    tool_name = full_name.substr(prefix.size());
                }
            }
            if (best_len == 0) {
                ext_key = name_to_key(full_name.substr(0, dd_pos));
                tool_name = full_name.substr(dd_pos + 2);
            }
        } else {
            for (const auto& [key, ext_entry] : extensions_) {
                for (const auto& tool : ext_entry->tools_cache) {
                    if (tool.name == full_name) {
                        ext_key = key;
                        tool_name = full_name;
                        break;
                    }
                }
                if (!ext_key.empty()) break;
            }
        }

        if (ext_key.empty()) {
            return Result<nlohmann::json>::err(
                make_error(ErrorCode::ExtensionError, "未找到工具对应的扩展: " + full_name));
        }

        auto it = extensions_.find(ext_key);
        if (it == extensions_.end()) {
            return Result<nlohmann::json>::err(
                make_error(ErrorCode::ExtensionError, "扩展未找到: " + ext_key));
        }

        // Hold a shared reference so the entry stays alive even if another
        // thread removes the extension while the tool call is in flight.
        entry = it->second;
    }

    if (entry->builtin) {
        spdlog::info("Calling builtin tool: {} (via {})", tool_name, entry->config.name);
        return entry->builtin->call_tool(tool_name, arguments);
    }

    if (entry->client) {
        spdlog::info("Calling MCP tool: {} (via {})", tool_name, entry->config.name);
        return entry->client->call_tool(tool_name, arguments);
    }

    return Result<nlohmann::json>::err(
        make_error(ErrorCode::ExtensionError, "扩展没有可用客户端: " + ext_key));
}

} // namespace goose
