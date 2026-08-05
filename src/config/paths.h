#pragma once

#include <string>
#include <filesystem>

namespace goose {
namespace paths {

// 所有持久化数据统一放在 ~/.kacli/ 根目录下，按功能分子目录：
//   config.yaml / secrets.yaml / prompts/ / hints/ / agents/ / sessions/ / recipes/ / logs/
// GOOSE_CONFIG_DIR 环境变量可覆盖根目录（测试隔离用）。
std::filesystem::path config_dir();

// 与 config_dir() 相同（统一根目录），保留旧名避免语义混淆。
std::filesystem::path data_dir();

// 用户主目录 (HOME / USERPROFILE)
std::filesystem::path home_dir();

// 会话目录: ~/.kacli/sessions/
std::filesystem::path session_dir();

// 日志目录: ~/.kacli/logs/
std::filesystem::path log_dir();

// 配置文件路径: ~/.kacli/config.yaml
std::filesystem::path config_file();

} // namespace paths
} // namespace goose
