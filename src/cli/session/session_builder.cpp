#include "session_builder.h"
#include "../../config/config.h"
#include "../../provider/model_context.h"
#include "../../provider/provider_registry.h"
#include <spdlog/spdlog.h>
#include <iostream>
#include <filesystem>

namespace goose {
namespace cli {

static Result<std::string> find_session_by_name(SessionManager& mgr, const std::string& name) {
    auto sessions = mgr.list_sessions();
    if (!sessions) return Result<std::string>::err(sessions.error());

    for (const auto& s : *sessions) {
        if (s.name == name) {
            return Result<std::string>::ok(s.id);
        }
    }
    return Result<std::string>::err(make_error(ErrorCode::SessionError, "Session not found: " + name));
}

Result<CliSession> build_session(const SessionBuilderConfig& config) {
    auto& cfg = Config::global();

    std::string provider_name;
    if (config.provider_name) {
        provider_name = *config.provider_name;
    } else {
        auto p = cfg.get_goose_provider();
        if (p) {
            provider_name = *p;
        } else {
            provider_name = "openai";
        }
    }

    std::string model_name;
    if (config.model_name) {
        model_name = *config.model_name;
    } else {
        auto m = cfg.get_goose_model();
        if (m) {
            model_name = *m;
        } else {
            model_name = "gpt-4o";
        }
    }

    spdlog::info("Provider: {}, Model: {}", provider_name, model_name);

    auto provider = ProviderRegistry::instance().create(provider_name);
    if (!provider) {
        return Result<CliSession>::err(Error{
            ErrorCode::ConfigError,
            "未知提供商: " + provider_name
        });
    }

    ModelConfig model_config;
    model_config.model_name = model_name;
    model_config.context_limit = resolve_context_limit(model_name);
    model_config.reasoning_effort = cfg.get_reasoning_effort();

    auto extension_manager = std::make_unique<ExtensionManager>();

    // 默认注册内置 developer 扩展，保证工具始终可用；add_extension 幂等，
    // 配置里重复声明也不会重复注册。
    extension_manager->add_extension(ExtensionConfig::builtin("developer"));

    for (const auto& ext_name : config.extensions) {
        auto ext_config = ExtensionConfig::stdio(ext_name, ext_name, {});
        extension_manager->add_extension(ext_config);
    }

    auto config_exts = Config::global().get_extensions();
    for (auto& ext_config : config_exts) {
        extension_manager->add_extension(std::move(ext_config));
    }

    SessionManager& session_mgr = SessionManager::instance();
    std::string session_id;
    bool is_resume = false;

    if (config.session_id) {
        session_id = *config.session_id;
        is_resume = true;
    } else if (config.resume && config.name) {
        auto found = find_session_by_name(session_mgr, *config.name);
        if (!found) {
            return Result<CliSession>::err(Error{
                ErrorCode::SessionError,
                "Session not found: " + *config.name
            });
        }
        session_id = *found;
        is_resume = true;
    } else if (config.name) {
        auto found = find_session_by_name(session_mgr, *config.name);
        if (found) {
            session_id = *found;
            is_resume = true;
        } else {
            auto cwd = std::filesystem::current_path();
            auto new_session = session_mgr.create_session(cwd, *config.name);
            if (!new_session) {
                return Result<CliSession>::err(Error{
                    ErrorCode::SessionError,
                    "创建会话失败"
                });
            }
            session_id = new_session->id;
        }
    } else {
        auto cwd = std::filesystem::current_path();
        auto new_session = session_mgr.create_session(cwd, "cli_session");
        if (!new_session) {
            return Result<CliSession>::err(Error{
                ErrorCode::SessionError,
                "创建会话失败"
            });
        }
        session_id = new_session->id;
    }

    auto session_result = session_mgr.get_session(session_id, true);
    if (!session_result) {
        return Result<CliSession>::err(Error{
            ErrorCode::SessionError,
            "Failed to load session: " + session_id
        });
    }

    AgentConfig agent_config;
    agent_config.session_manager = &session_mgr;
    agent_config.additional_system_prompt = config.additional_system_prompt;
    agent_config.working_dir = std::filesystem::current_path().string();

    auto agent = std::make_unique<Agent>(
        std::move(provider),
        agent_config,
        std::move(extension_manager)
    );

    agent->set_model_config(model_config);

    session_mgr.update_session(session_id, provider_name);

    CliSession session(std::move(agent), session_id);
    session.set_quiet(config.quiet);
    session.set_max_turns(config.max_turns);
    session.set_resumed(is_resume);
    session.set_output_format(config.output_format);

    if (is_resume && session_result->conversation) {
        session.set_conversation(*session_result->conversation);
    }

    return Result<CliSession>::ok(std::move(session));
}

}} // namespace goose::cli
