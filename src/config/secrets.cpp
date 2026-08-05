#include "secrets.h"
#include "paths.h"
#include "../utils/format.h"
#include <yaml-cpp/yaml.h>
#include <spdlog/spdlog.h>
#include <fstream>
#include <filesystem>
#include <cstdlib>

namespace goose {

Secrets& Secrets::global() {
    static Secrets secrets;
    return secrets;
}

void Secrets::load() const {
    if (loaded_) return;

    auto path = paths::config_dir() / "secrets.yaml";
    if (!std::filesystem::exists(path)) {
        spdlog::debug("No secrets file: {}", path.string());
        loaded_ = true;
        return;
    }

    // Secrets hold API keys; restrict access to the owning user even when the
    // file was created with a lax umask.
    utils::set_file_permissions_private(path.string());

    try {
        auto node = YAML::LoadFile(path.string());
        if (node.IsMap()) {
            for (auto it = node.begin(); it != node.end(); ++it) {
                std::string key = it->first.as<std::string>();
                std::string value = it->second.as<std::string>();
                secrets_[key] = value;
            }
        }
        spdlog::debug("Loaded {} secrets from {}", secrets_.size(), path.string());
        loaded_ = true;
    } catch (const YAML::Exception& e) {
        spdlog::warn("Failed to parse secrets file: {}", e.what());
        loaded_ = true;
    }
}

Result<std::string> Secrets::get_secret(const std::string& key) const {
    std::string upper_key = key;
    std::transform(upper_key.begin(), upper_key.end(), upper_key.begin(), ::toupper);

    const char* env_val = std::getenv(upper_key.c_str());
    if (env_val) return Result<std::string>::ok(std::string(env_val));

    const char* env_val2 = std::getenv(key.c_str());
    if (env_val2) return Result<std::string>::ok(std::string(env_val2));

    std::lock_guard<std::mutex> lock(mutex_);
    load();

    auto it = secrets_.find(key);
    if (it != secrets_.end()) {
        return Result<std::string>::ok(it->second);
    }

    return Result<std::string>::err(make_error(ErrorCode::ConfigError, "Secret not found: " + key));
}

Result<void> Secrets::write(const std::function<void(YAML::Node&)>& mutate) {
    auto path = paths::config_dir() / "secrets.yaml";
    try {
        std::filesystem::create_directories(path.parent_path());

        YAML::Node node;
        if (std::filesystem::exists(path)) {
            node = YAML::LoadFile(path.string());
            if (!node.IsMap()) {
                node = YAML::Node(YAML::NodeType::Map);
            }
        }
        mutate(node);

        // Atomic write: temp file + rename, so a crash mid-write never leaves
        // a truncated secrets file behind.
        auto tmp_path = path;
        tmp_path += ".tmp";
        std::ofstream ofs(tmp_path);
        if (!ofs.is_open()) {
            return Result<void>::err(
                make_error(ErrorCode::IoError, "无法写入密钥文件: " + tmp_path.string()));
        }
        ofs << node;
        ofs.close();

        utils::set_file_permissions_private(tmp_path.string());

        std::error_code ec;
        std::filesystem::rename(tmp_path, path, ec);
        if (ec) {
            std::ofstream direct(path);
            direct << node;
            std::filesystem::remove(tmp_path);
        }
        return Result<void>::ok();
    } catch (const YAML::Exception& e) {
        return Result<void>::err(
            make_error(ErrorCode::ConfigError, std::string("写入密钥失败: ") + e.what()));
    }
}

Result<void> Secrets::set_secret(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    load();
    auto result = write([&](YAML::Node& node) { node[key] = value; });
    if (result) {
        secrets_[key] = value;
    }
    return result;
}

Result<void> Secrets::remove_secret(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    load();
    auto result = write([&](YAML::Node& node) { node.remove(key); });
    if (result) {
        secrets_.erase(key);
    }
    return result;
}

} // namespace goose
