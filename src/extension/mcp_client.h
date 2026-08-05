#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include "../utils/error.h"
#include "mcp_types.h"

namespace goose {

class McpClient {
public:
    McpClient();
    ~McpClient();

    static Result<std::unique_ptr<McpClient>>
    spawn(const std::string& cmd,
          const std::vector<std::string>& args,
          const std::unordered_map<std::string, std::string>& envs = {});

    Result<void> initialize();
    Result<std::vector<mcp::ToolInfo>> list_tools();
    Result<nlohmann::json> call_tool(const std::string& name,
                                           const nlohmann::json& arguments);
    void shutdown();

private:
    Result<nlohmann::json> send_request(const std::string& method,
                                         const nlohmann::json& params = nlohmann::json::object());

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace goose
