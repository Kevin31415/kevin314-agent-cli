#pragma once

#include <vector>
#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>
#include "extension_config.h"
#include "mcp_client.h"
#include "developer_extension.h"
#include "../core/tool.h"
#include "../utils/error.h"

namespace goose {

struct ExtensionEntry {
    ExtensionConfig config;
    std::unique_ptr<McpClient> client;
    std::unique_ptr<DeveloperExtension> builtin;
    std::vector<Tool> tools_cache;
};

class ExtensionManager {
public:
    ExtensionManager() = default;
    ~ExtensionManager();

    void add_extension(ExtensionConfig config);

    Result<std::vector<Tool>> list_tools() const;
    Result<nlohmann::json> call_tool(const std::string& full_name,
                                      const nlohmann::json& arguments);

private:
    static std::string name_to_key(const std::string& name);
    void add_builtin_developer();
    std::vector<Tool> fetch_tools_for_entry(const ExtensionEntry& entry) const;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<ExtensionEntry>> extensions_;
};

} // namespace goose
