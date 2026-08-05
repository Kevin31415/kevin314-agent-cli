#include "config/paths.h"
#include <cstdlib>

#ifdef _WIN32
    #include <windows.h>
    #include <shlobj.h>
    #pragma comment(lib, "shell32.lib")
#else
    #include <unistd.h>
#endif

namespace goose {
namespace paths {

static std::filesystem::path get_home_dir() {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
    if (home) return home;
    const char* drive = std::getenv("HOMEDRIVE");
    const char* path = std::getenv("HOMEPATH");
    if (drive && path) return std::string(drive) + path;
    return "C:\\Users\\Default";
#else
    const char* home = std::getenv("HOME");
    if (home) return home;
    const char* user = std::getenv("USER");
    if (user) return std::string("/home/") + user;
    return "/tmp";
#endif
}

// 所有持久化数据的唯一根目录 ~/.kacli/：
//   config.yaml     主配置
//   secrets.yaml    API 密钥
//   prompts/        提示词模板覆盖
//   hints/          全局提示 (.goosehints / AGENTS.md)
//   agents/         全局 agent 指令 (AGENTS.md)
//   sessions/       会话数据库
//   recipes/        配方
//   logs/           日志
// GOOSE_CONFIG_DIR 可覆盖根目录（测试隔离用）。
static std::filesystem::path kacli_root() {
    const char* override = std::getenv("GOOSE_CONFIG_DIR");
    if (override && *override) return std::filesystem::path(override);
    return std::filesystem::path(get_home_dir()) / ".kacli";
}

std::filesystem::path config_dir() {
    return kacli_root();
}

std::filesystem::path home_dir() {
    return get_home_dir();
}

std::filesystem::path data_dir() {
    return kacli_root();
}

std::filesystem::path session_dir() {
    return kacli_root() / "sessions";
}

std::filesystem::path config_file() {
    return kacli_root() / "config.yaml";
}

std::filesystem::path log_dir() {
    return kacli_root() / "logs";
}

} // namespace paths
} // namespace goose
