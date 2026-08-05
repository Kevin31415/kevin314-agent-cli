#include "extension/extension_config.h"

namespace goose {

ExtensionConfig ExtensionConfig::stdio(const std::string& name,
                                        const std::string& cmd,
                                        std::vector<std::string> args) {
    ExtensionConfig config;
    config.type = Type::Stdio;
    config.name = name;
    config.cmd = std::move(cmd);
    config.args = std::move(args);
    return config;
}

ExtensionConfig ExtensionConfig::builtin(const std::string& name) {
    ExtensionConfig config;
    config.type = Type::Builtin;
    config.name = name;
    config.description = name;
    return config;
}

ExtensionConfig ExtensionConfig::streamable_http(const std::string& name,
                                                    const std::string& uri) {
    ExtensionConfig config;
    config.type = Type::StreamableHttp;
    config.name = name;
    config.uri = uri;
    return config;
}

} // namespace goose
