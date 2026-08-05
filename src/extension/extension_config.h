#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <cstdint>

namespace goose {

struct ExtensionConfig {
    enum class Type { Stdio, Builtin, Platform, StreamableHttp };

    Type type = Type::Builtin;
    std::string name;
    std::string description;

    // Stdio specific
    std::string cmd;
    std::vector<std::string> args;
    std::unordered_map<std::string, std::string> envs;

    // StreamableHttp specific
    std::string uri;

    // Common
    std::optional<uint64_t> timeout;
    std::optional<bool> bundled;

    // 工具过滤 (可选)
    std::vector<std::string> available_tools;

    // 工厂方法
    static ExtensionConfig stdio(const std::string& name,
                                  const std::string& cmd,
                                  std::vector<std::string> args = {});

    static ExtensionConfig builtin(const std::string& name);

    static ExtensionConfig streamable_http(const std::string& name,
                                            const std::string& uri);
};

} // namespace goose
