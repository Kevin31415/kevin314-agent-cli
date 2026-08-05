#include "provider/provider_registry.h"
#include "provider/openai.h"
#include "provider/anthropic.h"
#include "../config/config.h"
#include <algorithm>
#include <spdlog/spdlog.h>

namespace goose {

ProviderRegistry& ProviderRegistry::instance() {
    static ProviderRegistry registry;
    return registry;
}

std::shared_ptr<Provider> ProviderRegistry::create(const std::string& name) {
    if (name == "openai" || name == "anthropic") {
        auto entry = Config::global().get_provider(name);
        if (entry && entry->configured && !entry->base_url.empty()) {
            if (name == "anthropic") {
                return std::make_shared<AnthropicProvider>(name, entry->base_url);
            }
            return std::make_shared<OpenAiProvider>(name, entry->base_url);
        }
        if (name == "anthropic") {
            return std::make_shared<AnthropicProvider>();
        }
        return std::make_shared<OpenAiProvider>();
    }

    // 自定义提供商：从 config.yaml 的 providers.<name> 读取类型与 base_url
    auto entry = Config::global().get_provider(name);
    if (entry && entry->configured) {
        if (entry->type == "anthropic") {
            return std::make_shared<AnthropicProvider>(name, entry->base_url);
        }
        if (entry->type == "openai" || entry->type == "openai_compatible") {
            return std::make_shared<OpenAiProvider>(name, entry->base_url);
        }
        spdlog::warn("Unknown provider type '{}' for provider '{}'", entry->type, name);
        return nullptr;
    }

    spdlog::warn("Unknown provider '{}' is not configured; refusing to silently fall back to OpenAI", name);
    return nullptr;
}

std::vector<std::string> ProviderRegistry::available_providers() const {
    std::vector<std::string> names = {"openai", "anthropic"};
    auto configured = Config::global().get_configured_providers();
    for (const auto& name : configured) {
        if (std::find(names.begin(), names.end(), name) == names.end()) {
            names.push_back(name);
        }
    }
    return names;
}

} // namespace goose
