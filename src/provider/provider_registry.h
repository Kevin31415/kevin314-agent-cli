#pragma once

#include <memory>
#include <string>
#include "base.h"

namespace goose {

class ProviderRegistry {
public:
    static ProviderRegistry& instance();

    // 创建 provider
    std::shared_ptr<Provider> create(const std::string& name);

    // 获取所有可用 provider 名称
    std::vector<std::string> available_providers() const;

private:
    ProviderRegistry() = default;
};

} // namespace goose
