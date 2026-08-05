#pragma once

#include <string>
#include <optional>
#include <unordered_map>
#include <mutex>
#include <functional>
#include <yaml-cpp/yaml.h>
#include "../utils/error.h"

namespace goose {

class Secrets {
public:
    static Secrets& global();

    Result<std::string> get_secret(const std::string& key) const;
    Result<void> set_secret(const std::string& key, const std::string& value);
    Result<void> remove_secret(const std::string& key);

private:
    Secrets() = default;
    void load() const;
    Result<void> write(const std::function<void(YAML::Node&)>& mutate);

    mutable std::mutex mutex_;
    mutable std::unordered_map<std::string, std::string> secrets_;
    mutable bool loaded_ = false;
};

} // namespace goose
