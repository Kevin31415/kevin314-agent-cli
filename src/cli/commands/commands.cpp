#include "commands.h"
#includePath "../session/session_builder.h"
#includePath "../../config/config.h"
#include "../../config/paths.h"
#includePath "../../config/secrets.h"
#include "../../provider/provider_registry.h"
#include "../../utils/format.h"
#include "../../utils/prompt_template.h"
#include <spdlog/spdlog.h>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <yaml-cpp/yaml.h>

namespace goose {
namespace cli {

static int run_session_impl(const CliArgs& args, bool stdin_fallback = false) {
    SessionBuilderConfig config;
    config.provider_name = args.provider;
    config.model_name = args.model;
    config.debug = args.debug;
    config.max_turns = args.max_turns.value_or(0);
    config.extensions = args.extensions;
    config.text = args.text;
    config.resume = args.resume;
    config.name = args.name;
    config.session_id = args.session_id;
    config.output_format = args.output_format;

    if (args.output_format != "text" && args.output_format != "json") {
        std::cerr << "错误: 无效的 --output-format '" << args.output_format
                  << "' (应为 text|json)\n";
        return 1;
    }

    if (args.system_prompt) {
        auto path = std::filesystem::path(*args.system_prompt);
        if (!std::filesystem::exists(path)) {
            std::cerr << "错误: 系统提示词文件不存在: " << *args.system_prompt << "\n";
            return 1;
        }
        std::ifstream ifs(path);
        config.additional_system_prompt = std::string(
            (std::istreambuf_iterator<char>(ifs)),
            std::istreambuf_iterator<char>());
    }

    if (args.recipe) {
        auto path = paths::data_dir() / "recipes" / (*args.recipe + ".yaml");
        if (!std::filesystem::exists(path)) {
            std::cerr << "错误: 配方不存在: " << *args.recipe
                      << " (已搜索 " << path << ")\n";
            return 1;
        }
        std::ifstream ifs(path);
        config.text = std::string(
            (std::istreambuf_iterator<char>(ifs)),
            std::istreambuf_iterator<char>());
    } else if (args.instructions) {
        auto path = std::filesystem::path(*args.instructions);
        if (std::filesystem::exists(path)) {
            std::ifstream ifs(path);
            config.text = std::string(
                (std::istreambuf_iterator<char>(ifs)),
                std::istreambuf_iterator<char>());
        } else {
            config.text = *args.instructions;
        }
    }

    auto session_result = build_session(config);
    if (!session_result) {
        spdlog::error("Failed to build session: {}", session_result.error().message);
        std::cerr << "错误: " << session_result.error().message << "\n";
        return 1;
    }

    auto session = std::move(*session_result);

    if (config.text) {
        return session.run_headless(*config.text);
    }

    if (stdin_fallback) {
        std::string input;
        std::cout << "> " << std::flush;
        if (std::getline(std::cin, input) && !input.empty()) {
            return session.run_headless(input);
        }
        return 0;
    }

    return session.run_interactive();
}

int run_session_cmd(const CliArgs& args) {
    return run_session_impl(args, false);
}

int run_run_cmd(const CliArgs& args) {
    return run_session_impl(args, true);
}

static bool is_builtin_provider(const std::string& name) {
    return name == "openai" || name == "anthropic";
}

int run_configure_cmd(const CliArgs& /*args*/) {
    auto& cfg = Config::global();

    std::cout << "Kevin314 智能体命令行配置\n";
    std::cout << "===========================\n\n";

    auto config_file = paths::config_file();
    bool exists = std::filesystem::exists(config_file);

    if (exists) {
        std::cout << "配置文件: " << config_file << "\n\n";
        std::cout << "当前提供商: " << cfg.get_active_provider() << "\n";
        std::cout << "当前模型:   " << cfg.get_active_model() << "\n";
        std::cout << "当前模式:   " << to_string(cfg.get_goose_mode()) << "\n";
        const std::string cur_effort = cfg.get_reasoning_effort();
        std::cout << "思考等级:   " << (cur_effort.empty() ? "不设置" : cur_effort) << "\n\n";
    } else {
        std::cout << "未找到配置文件，将创建: " << config_file << "\n\n";
    }

    auto& registry = ProviderRegistry::instance();
    auto known = registry.available_providers();
    std::string provider_hint = known.empty() ? "" : "[" + known.front() + "]";
    for (size_t i = 1; i < known.size(); ++i) {
        provider_hint += "/" + known[i];
    }

    std::string provider;
    std::cout << "提供商 " << provider_hint << " (" << cfg.get_active_provider() << "): ";
    std::getline(std::cin, provider);
    if (provider.empty()) provider = cfg.get_active_provider();

    std::string type = "openai";
    std::string base_url;
    if (!is_builtin_provider(provider)) {
        std::cout << "兼容类型 [openai/anthropic] (openai): ";
        std::getline(std::cin, type);
        if (type != "anthropic") type = "openai";
        std::cout << "API 地址 (留空使用默认): ";
        std::getline(std::cin, base_url);
    } else if (provider == "openai") {
        std::cout << "API 地址 (留空使用官方 https://api.openai.com): ";
        std::getline(std::cin, base_url);
    }

    std::string model;
    std::cout << "模型 (" << cfg.get_active_model() << "): ";
    std::getline(std::cin, model);
    if (model.empty()) model = cfg.get_active_model();

    std::string mode;
    std::cout << "模式 [auto/approve/smart_approve] (" << to_string(cfg.get_goose_mode()) << "): ";
    std::getline(std::cin, mode);
    if (mode.empty()) mode = to_string(cfg.get_goose_mode());

    // 思考等级：抽象档位 low/medium/high/max，留空不设置。
    // 各提供商在请求构造时自行翻译成自己的单位（OpenAI reasoning_effort、
    // Anthropic budget_tokens 等），用户无需知道提供商方言。
    std::string effort = cfg.get_reasoning_effort();
    std::cout << "思考等级 [low/medium/high/max，留空不设置] (" << (effort.empty() ? "不设置" : effort) << "): ";
    std::string effort_input;
    std::getline(std::cin, effort_input);
    std::transform(effort_input.begin(), effort_input.end(), effort_input.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    while (!effort_input.empty() && effort_input != "low" && effort_input != "medium" &&
           effort_input != "high" && effort_input != "max") {
        std::cout << "无效思考等级 '" << effort_input << "'，请输入 low/medium/high/max 或留空: ";
        std::getline(std::cin, effort_input);
        std::transform(effort_input.begin(), effort_input.end(), effort_input.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    }

    YAML::Node config;
    if (exists) {
        try { config = YAML::LoadFile(config_file.string()); }
        catch (...) {
            spdlog::warn("Config file is not valid YAML and will be rewritten: {}", config_file.string());
        }
    }

    config["active_provider"] = provider;
    config["GOOSE_PROVIDER"] = provider;
    if (!model.empty()) {
        config["GOOSE_MODEL"] = model;
    }
    config["GOOSE_MODE"] = mode;
    if (!effort_input.empty()) {
        config["KACLI_REASONING_EFFORT"] = effort_input;
    }

    if (!config["providers"]) {
        config["providers"] = YAML::Node(YAML::NodeType::Map);
    }

    if (!config["providers"][provider]) {
        config["providers"][provider] = YAML::Node(YAML::NodeType::Map);
    }
    config["providers"][provider]["model"] = model;
    config["providers"][provider]["enabled"] = true;
    config["providers"][provider]["configured"] = true;
    if (!is_builtin_provider(provider)) {
        config["providers"][provider]["type"] = type;
    }
    if (!base_url.empty()) {
        config["providers"][provider]["base_url"] = base_url;
    }

    try {
        std::filesystem::create_directories(config_file.parent_path());
        std::ofstream ofs(config_file);
        if (!ofs.is_open()) {
            std::cerr << "保存配置失败: 无法打开文件 " << config_file << "\n";
            return 1;
        }
        ofs << config;
        if (ofs.fail()) {
            std::cerr << "保存配置失败: 写入错误 " << config_file << "\n";
            return 1;
        }
        std::cout << "\n配置已保存到: " << config_file << "\n";
        std::cout << "提供商: " << provider << "\n";
        std::cout << "模型:   " << model << "\n";
        std::cout << "模式:   " << mode << "\n";
        std::cout << "思考等级: " << (effort_input.empty() ? "不设置" : effort_input) << "\n";
    } catch (const std::exception& e) {
        std::cerr << "保存配置失败: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

int run_info_cmd(const CliArgs& /*args*/) {
    auto& registry = ProviderRegistry::instance();
    auto providers = registry.available_providers();
    std::string providers_str;
    for (size_t i = 0; i < providers.size(); ++i) {
        if (i > 0) providers_str += ", ";
        providers_str += providers[i];
    }

    std::cout << "Kevin314 智能体命令行 (kacli) v0.1.0\n";
    std::cout << "提供商: " << providers_str << "\n";
    std::cout << "扩展: developer (内置)\n";
    return 0;
}

int run_doctor_cmd(const CliArgs& /*args*/) {
    int issues = 0;

    std::cout << "Kevin314 智能体命令行诊断 v0.1.0\n";
    std::cout << "===================================\n\n";

    // Check config directory
    auto config_dir = paths::config_dir();
    if (std::filesystem::exists(config_dir)) {
        std::cout << "[通过] 配置目录: " << config_dir << "\n";
    } else {
        std::cout << "[警告] 配置目录不存在: " << config_dir << "\n";
        issues++;
    }

    // Check data directory
    auto data_dir = paths::data_dir();
    if (std::filesystem::exists(data_dir)) {
        std::cout << "[通过] 数据目录: " << data_dir << "\n";
    } else {
        std::cout << "[警告] 数据目录不存在: " << data_dir << "\n";
        issues++;
    }

    // Check session directory
    auto session_dir = paths::session_dir();
    if (std::filesystem::exists(session_dir)) {
        std::cout << "[通过] 会话目录: " << session_dir << "\n";
    } else {
        std::cout << "[信息] 会话目录尚未创建: " << session_dir << "\n";
    }

    // Check config file
    auto config_file = paths::config_file();
    if (std::filesystem::exists(config_file)) {
        std::cout << "[通过] 配置文件: " << config_file << "\n";
    } else {
        std::cout << "[信息] 配置文件不存在 (使用默认值): " << config_file << "\n";
    }

    // Check environment variables
    std::cout << "\n--- 环境变量 ---\n";

    auto check_env = [&](const std::string& name, const std::string& legacy_name, bool required) {
        std::string value;
        bool from_secrets = false;
        const char* val = std::getenv(name.c_str());
        if (!val) val = std::getenv(legacy_name.c_str());
        if (val) {
            value = val;
        } else {
            auto secret = Secrets::global().get_secret(name);
            if (secret) {
                value = *secret;
                from_secrets = true;
            }
        }
        if (!value.empty()) {
            std::string display(value);
            if ((name.find("KEY") != std::string::npos || name.find("SECRET") != std::string::npos) && display.size() >= 8) {
                display = display.substr(0, 4) + "..." + display.substr(display.size() - 4);
            }
            std::cout << "[通过] " << name << " = " << display << (from_secrets ? " (来自 secrets.yaml)" : "") << "\n";
        } else {
            if (required) {
                std::cout << "[警告] " << name << " 未设置 (必需)\n";
                issues++;
            } else {
                std::cout << "[信息] " << name << " 未设置 (可选)\n";
            }
        }
    };

    check_env("OPENAI_API_KEY", "", true);
    check_env("OPENAI_BASE_URL", "", false);
    check_env("ANTHROPIC_API_KEY", "", false);
    check_env("ANTHROPIC_BASE_URL", "", false);
    check_env("GOOSE_PROVIDER", "KACLI_PROVIDER", false);
    check_env("GOOSE_MODEL", "KACLI_MODEL", false);
    check_env("GOOSE_MODE", "KACLI_MODE", false);

    // Check providers
    std::cout << "\n--- 提供商 ---\n";
    auto& registry = ProviderRegistry::instance();
    for (const auto& name : registry.available_providers()) {
        auto provider = registry.create(name);
        if (provider) {
            std::cout << "[通过] " << name << " 可用\n";
        } else {
            std::cout << "[警告] " << name << " 初始化失败\n";
            issues++;
        }
    }

    // Check config
    std::cout << "\n--- 配置 ---\n";
    auto& cfg = Config::global();
    auto active_provider = cfg.get_active_provider();
    auto active_model = cfg.get_active_model();
    std::cout << "[信息] 当前提供商: " << active_provider << "\n";
    std::cout << "[信息] 当前模型: " << active_model << "\n";
    std::cout << "[信息] 模式: " << to_string(cfg.get_goose_mode()) << "\n";

    auto exts = cfg.get_extensions();
    std::cout << "[信息] 已配置扩展: " << exts.size() << "\n";

    std::cout << "\n--- 总结 ---\n";
    if (issues == 0) {
        std::cout << "全部检查通过! Kevin314 智能体命令行已就绪。\n";
    } else {
        std::cout << "发现 " << issues << " 个问题，请修复以上警告。\n";
    }

    return issues > 0 ? 1 : 0;
}

int run_template_cmd(const CliArgs& args) {
    auto& templates = PromptTemplate::global();

    if (args.template_action == "list") {
        auto infos = templates.list_templates();
        if (infos.empty()) {
            std::cout << "暂无模板。\n";
            return 0;
        }
        std::cout << "模板名称                   | 状态\n";
        std::cout << "---------------------------+----------\n";
        for (const auto& info : infos) {
            std::cout << std::left << std::setw(28) << info.name.substr(0, 28) << "| "
                      << (info.is_customized ? "已自定义" : "默认") << "\n";
        }
        return 0;
    }

    if (args.template_action == "show") {
        if (args.template_name.empty()) {
            std::cerr << "用法: kacli template show <模板名称>\n";
            return 1;
        }
        auto info = templates.get_template(args.template_name);
        if (info.name.empty()) {
            std::cerr << "模板不存在: " << args.template_name << "\n";
            return 1;
        }
        std::cout << (info.user_content.empty() ? info.default_content : info.user_content);
        std::cout << "\n";
        return 0;
    }

    if (args.template_action == "save") {
        if (args.template_name.empty() || args.template_content.empty()) {
            std::cerr << "用法: kacli template save <模板名称> \"模板内容\"\n";
            return 1;
        }
        templates.save_template(args.template_name, args.template_content);
        std::cout << "已保存自定义模板: " << args.template_name << "\n";
        return 0;
    }

    if (args.template_action == "reset") {
        if (args.template_name.empty()) {
            std::cerr << "用法: kacli template reset <模板名称>\n";
            return 1;
        }
        templates.reset_template(args.template_name);
        std::cout << "已恢复默认模板: " << args.template_name << "\n";
        return 0;
    }

    std::cerr << "未知的 template 操作。可用: list, show, save, reset\n";
    return 1;
}

static std::string mask_key(const std::string& value) {
    if (value.size() <= 8) {
        return "****";
    }
    return value.substr(0, 4) + "****" + value.substr(value.size() - 4);
}

int run_key_cmd(const CliArgs& args) {
    auto& secrets = Secrets::global();
    constexpr const char* kApiKey = "OPENAI_API_KEY";

    if (args.key_unset) {
        if (std::getenv(kApiKey)) {
            std::cerr << "错误: OPENAI_API_KEY 来自环境变量，请取消环境变量设置后重试。\n";
            return 1;
        }
        auto result = secrets.remove_secret(kApiKey);
        if (!result) {
            std::cerr << "删除密钥失败: " << result.error().message << "\n";
            return 1;
        }
        std::cout << "已删除 API 密钥。\n";
        return 0;
    }

    if (args.key_value) {
        if (args.key_value->empty()) {
            std::cerr << "错误: 密钥值不能为空。\n";
            return 1;
        }
        auto result = secrets.set_secret(kApiKey, *args.key_value);
        if (!result) {
            std::cerr << "保存密钥失败: " << result.error().message << "\n";
            return 1;
        }
        std::cout << "API 密钥已保存到 "
                  << (paths::config_dir() / "secrets.yaml").string() << "\n";
        return 0;
    }

    auto result = secrets.get_secret(kApiKey);
    if (!result) {
        std::cout << "未配置 API 密钥。\n"
                  << "用法: kacli key <你的密钥>\n"
                  << "      或设置环境变量 " << kApiKey << "\n";
        return 1;
    }
    std::cout << "当前 API 密钥: " << mask_key(*result) << "\n";
    return 0;
}

int run_sessions_cmd(const CliArgs& args) {
    auto& session_mgr = SessionManager::instance();

    if (args.session_delete) {
        if (!args.name) {
            std::cerr << "用法: kacli sessions -d <会话名称或ID>\n";
            return 1;
        }
        auto result = session_mgr.delete_session(*args.name);
        if (result) {
            std::cout << "已删除会话: " << *args.name << "\n";
            return 0;
        } else {
            std::cerr << "删除会话失败: " << result.error().message << "\n";
            return 1;
        }
    }

    auto sessions = session_mgr.list_sessions();
    if (!sessions) {
        std::cerr << "列出会话失败: " << sessions.error().message << "\n";
        return 1;
    }

    if (sessions->empty()) {
        std::cout << "暂无会话。\n";
        return 0;
    }

    std::cout << "ID                               | 名称                 | 提供商    | 更新时间\n";
    std::cout << "---------------------------------+----------------------+-------------+-------------------\n";
    for (const auto& s : *sessions) {
        std::cout << std::left
                  << std::setw(33) << s.id.substr(0, 33) << "| "
                  << std::setw(22) << s.name.substr(0, 22) << "| "
                  << std::setw(13) << (s.provider_name.empty() ? "-" : s.provider_name) << "| "
                  << utils::format_time(s.updated_at) << "\n";
    }

    std::cout << "\n共 " << sessions->size() << " 个会话。\n";
    return 0;
}

}} // namespace goose::cli
