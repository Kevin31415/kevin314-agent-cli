#pragma once

#include <string>
#include <vector>
#include <functional>
#include <nlohmann/json.hpp>
#include "../utils/error.h"
#include "../core/tool.h"

namespace goose {

struct ShellResult {
    std::string stdout_output;
    std::string stderr_output;
    int exit_code = -1;
    bool timed_out = false;
};

class DeveloperExtension {
public:
    DeveloperExtension();

    std::vector<Tool> list_tools() const;
    Result<nlohmann::json> call_tool(const std::string& name,
                                      const nlohmann::json& arguments,
                                      const std::string& working_dir = "");

private:
    Result<nlohmann::json> handle_write(const nlohmann::json& args, const std::string& cwd);
    Result<nlohmann::json> handle_edit(const nlohmann::json& args, const std::string& cwd);
    Result<nlohmann::json> handle_shell(const nlohmann::json& args, const std::string& cwd);
    Result<nlohmann::json> handle_tree(const nlohmann::json& args, const std::string& cwd);
    Result<nlohmann::json> handle_read(const nlohmann::json& args, const std::string& cwd);

    ShellResult run_shell(const std::string& command, uint64_t timeout_secs,
                          const std::string& working_dir);

    std::string working_dir_;
};

} // namespace goose
