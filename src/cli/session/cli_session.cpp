#include "cli_session.h"
#include "input.h"
#include "output.h"
#include "../../session/session_manager.h"
#include "../../config/config.h"
#include "../../core/goose_mode.h"
#include "../../core/compaction.h"
#include "../../provider/provider_registry.h"
#include "../../provider/model_context.h"
#include "../../utils/format.h"
#include <spdlog/spdlog.h>
#include <iostream>
#include <unistd.h>
#include <cstdlib>

namespace goose {
namespace cli {

CliSession::CliSession(std::unique_ptr<Agent> agent, const std::string& session_id)
    : agent_(std::move(agent)), session_id_(session_id) {}

static std::string build_compacted_text(const Conversation& conv) {
    std::string text;
    for (const auto& msg : conv.messages()) {
        text += format_message_for_compacting(msg) + "\n";
    }
    return text;
}

static const char* kHelpText =
    "可用命令:\n"
    "  /help              查看帮助\n"
    "  /stop              中断 AI 当前回复\n"
    "  /exit, /quit       退出会话\n"
    "  /clear             清空对话\n"
    "  /mode [auto|approve|smart-approve]  查看/设置运行模式\n"
    "  /model [provider/model]             查看/设置模型\n"
    "  /prompt [system|tiny]               查看/切换系统提示词方案\n"
    "  /compact           压缩对话\n"
    "  /history           查看会话历史\n";

CliSession::~CliSession() = default;

CliSession::CliSession(CliSession&&) noexcept = default;
CliSession& CliSession::operator=(CliSession&&) noexcept = default;

int CliSession::run_interactive() {
    spdlog::info("Starting interactive session: {}", session_id_);

    const char* no_tui = getenv("KACLI_NO_TUI");
    bool tui_allowed = !quiet_ && output_format_ != "json" && isatty(STDIN_FILENO) &&
                       (!no_tui || std::string(no_tui) == "0");
    if (tui_allowed) {
        ui_ = std::make_unique<TerminalUi>();
        tui_active_ = ui_->enter();
    }

    if (tui_active_) {
        ui_->set_session_info(session_id_, session_id_, agent_->provider()->get_name(),
                             agent_->model_config().model_name);
        ui_->set_busy_command_handler([this](const std::string& cmd) {
            if (cmd == "/stop") {
                agent_->interrupt();
                ui_->interrupt_confirm();
            } else if (cmd == "/help") {
                ui_->append_status_text(kHelpText);
            } else if (cmd == "/exit" || cmd == "/quit") {
                ui_->append_status_text("AI 正在工作，请先 /stop 或等待完成后再退出。");
            } else {
                ui_->append_status_text("AI 正在工作，该命令需等待完成后再使用（/stop 可中断）。");
            }
        });
        ui_->append_status_text(resumed_ ? "恢复会话: " + session_id_ : "开始新会话: " + session_id_);
        if (resumed_ && !messages_.is_empty()) {
            ui_->append_status_text("已加载 " + std::to_string(messages_.len()) + " 条历史消息。");
        }
        if (tui_active_) {
            ui_->start_refresh();
        }
        ui_->append_status_text("输入消息开始对话，/help 查看命令，输入 'quit' 退出。");
    } else if (!quiet_) {
        if (resumed_) {
            std::cout << "恢复会话: " << session_id_ << "\n";
            if (!messages_.is_empty()) {
                std::cout << "已加载 " << messages_.len() << " 条历史消息。\n";
            }
        } else {
            std::cout << "开始新会话: " << session_id_ << "\n";
        }
        std::cout << "输入消息开始对话，/help 查看命令，输入 'quit' 退出。\n";
    }

    while (true) {
        auto raw = tui_active_ ? ui_->read_line_input() : read_line("\n> ");
        auto parsed = parse_input(raw);

        switch (parsed.type) {
            case InputResult::Exit:
                if (tui_active_) {
                    ui_->stop_refresh();
                    ui_->append_status_text("再见!");
                } else if (!quiet_) {
                    std::cout << "再见!\n";
                }
                return 0;

            case InputResult::Retry:
                continue;

            case InputResult::SlashCommand:
                handle_slash_command(parsed);
                if (should_exit_) return 0;
                continue;

            case InputResult::Message:
                break;
        }

        Message user_msg = Message::user().with_text(parsed.text);
        auto& session_mgr = SessionManager::instance();
        session_mgr.add_message(session_id_, user_msg);
        messages_.push(user_msg);
        if (tui_active_) {
            ui_->append_user_message(parsed.text);
        }
        refresh_context_tokens();

        maybe_auto_compact();

        goose::SessionConfig sess;
        sess.max_turns = std::nullopt;
        sess.on_confirm = [this](const std::string& tool_name, const nlohmann::json& args) -> bool {
            std::string summary = tool_name;
            if (args.contains("command") && args["command"].is_string()) {
                summary += " " + args["command"].get<std::string>();
            } else if (args.contains("path") && args["path"].is_string()) {
                summary += " " + args["path"].get<std::string>();
            }
            if (tui_active_) {
                return ui_->confirm_tool(tool_name, summary);
            }
            std::cout << "\n\033[33m⚠ 批准执行工具: " << summary << "? [y/N] \033[0m" << std::flush;
            std::string input;
            std::getline(std::cin, input);
            return !input.empty() && (input[0] == 'y' || input[0] == 'Y');
        };

        if (tui_active_) {
            ui_->begin_assistant_turn();
            ui_->set_busy(true);
        }
        auto result = agent_->reply(
            sess,
            [this](AgentEvent event) { handle_agent_event(event); },
            messages_);
        if (tui_active_) {
            ui_->set_busy(false);
            if (agent_->interrupt_requested()) {
                ui_->notify_work_idle();
                ui_->append_status_text("已中断。");
            }
        }

        if (!result) {
            if (tui_active_) {
                ui_->append_status_text("\u9519\u8bef: " + result.error().message);
            } else {
                render_error(result.error().message);
            }
        } else {
            turn_count_++;
        }
        if (max_turns_ > 0 && turn_count_ >= max_turns_) {
            if (tui_active_) {
                ui_->append_status_text("已达到最大轮数。");
            } else if (!quiet_) {
                std::cout << "已达到最大轮数。\n";
            }
            break;
        }
    }

    return 0;
}

int CliSession::run_headless(const std::string& prompt) {
    spdlog::info("Running headless: {}", prompt);

    Message user_msg = Message::user().with_text(prompt);
    auto& session_mgr = SessionManager::instance();
    session_mgr.add_message(session_id_, user_msg);
    messages_.push(user_msg);

    std::optional<uint32_t> max_turns =
        max_turns_ > 0 ? std::optional<uint32_t>(static_cast<uint32_t>(max_turns_))
                       : std::nullopt;
    auto result = agent_->reply(
        goose::SessionConfig{max_turns, nullptr},
        [this](AgentEvent event) { handle_agent_event(event); },
        messages_);

    if (!result) {
        if (!quiet_) {
            render_error(result.error().message);
        }
        return 1;
    }
    return 0;
}

void CliSession::render_json_event(const AgentEvent& event) {
    nlohmann::json j = nlohmann::json::object();
    switch (event.type) {
        case AgentEventType::Message:
            if (event.msg) {
                j["type"] = "message";
                to_json(j["message"], *event.msg);
            }
            break;
        case AgentEventType::TextDelta:
            j["type"] = "text_delta";
            j["text"] = event.text_delta;
            break;
        case AgentEventType::ReasoningDelta:
            j["type"] = "reasoning_delta";
            j["text"] = event.reasoning_delta;
            break;
        case AgentEventType::Usage:
            if (event.provider_usage) {
                j["type"] = "usage";
                to_json(j["usage"], event.provider_usage->usage);
            }
            break;
    }
    if (!j.empty()) {
        std::cout << j.dump() << "\n" << std::flush;
    }
}

Result<void> CliSession::handle_agent_event(const AgentEvent& event) {
    if (tui_active_) {
        ui_->on_agent_event(event);
    } else if (output_format_ == "json") {
        render_json_event(event);
    } else if (!quiet_) {
        render_agent_event(event);
    }
    if (event.type == AgentEventType::Message && event.msg) {
        SessionManager::instance().add_message(session_id_, *event.msg);
        messages_.push(*event.msg);
        refresh_context_tokens();
    }
    return Result<void>::ok();
}

void CliSession::out(const std::string& s) {
    if (tui_active_) {
        ui_->append_status_text(s);
    } else if (!quiet_) {
        std::cout << s;
    }
}

void CliSession::refresh_context_tokens() {
    if (tui_active_) {
        ui_->set_context_tokens(estimate_conversation_tokens(messages_));
    }
}

void CliSession::maybe_auto_compact() {
    auto& cfg = Config::global();
    double threshold = kDefaultCompactionThreshold;
    if (auto threshold_str = cfg.get_param("GOOSE_AUTO_COMPACT_THRESHOLD"); threshold_str) {
        try {
            threshold = std::stod(*threshold_str);
        } catch (const std::exception&) {
        }
    }
    int64_t context_limit = agent_->model_config().context_limit.value_or(kDefaultContextLimit);

    if (!check_if_compaction_needed(messages_, threshold, context_limit)) {
        return;
    }

    auto compact_result = compact_messages(
        messages_, *agent_->provider(), agent_->model_config(), false);
    if (compact_result) {
        messages_ = compact_result->conversation;
        SessionManager::instance().replace_messages(session_id_, messages_.messages());
        if (tui_active_) {
            refresh_context_tokens();
            ui_->set_compact_result(build_compacted_text(messages_));
        }
        if (!quiet_) {
            out("\n(对话上下文已自动压缩)\n");
        }
    } else {
        spdlog::warn("Auto-compact failed: {}", compact_result.error().message);
    }
}

void CliSession::handle_slash_command(const ParsedInput& cmd) {
    if (cmd.command == "/help") {
        out(kHelpText);
    } else if (cmd.command == "/stop") {
        out("当前没有正在进行的任务。\n");
    } else if (cmd.command == "/clear") {
        messages_ = Conversation();
        SessionManager::instance().clear_messages(session_id_);
        refresh_context_tokens();
        out("对话已清空。\n");
    } else if (cmd.command == "/exit" || cmd.command == "/quit") {
        out("再见!\n");
        should_exit_ = true;
    } else if (cmd.command == "/mode") {
        auto& cfg = Config::global();
        if (cmd.args.empty()) {
            out(std::string("当前模式: ") + to_string(cfg.get_goose_mode()) + "\n");
        } else {
            auto parsed = goose_mode_from_string(cmd.args[0]);
            if (!parsed) {
                out("未知模式: " + cmd.args[0] + "（可选: auto/approve/smart_approve/chat/bypass_permissions）\n");
            } else {
                cfg.set_goose_mode(*parsed);
                out(std::string("模式已设置为: ") + to_string(*parsed) + "\n");
            }
        }
    } else if (cmd.command == "/model") {
        if (cmd.args.empty()) {
            auto& mc = agent_->model_config();
            std::string limit = mc.context_limit ? std::to_string(*mc.context_limit / 1000) + "k"
                                                 : "默认";
            out("当前提供商: " + agent_->provider()->get_name() + "\n"
                "当前模型: " + mc.model_name + "\n"
                "上下文上限: " + limit + "\n");
        } else {
            std::string model = cmd.args[0];
            std::string provider_name;
            std::string model_name = model;

            auto slash = model.find('/');
            if (slash != std::string::npos) {
                provider_name = model.substr(0, slash);
                model_name = model.substr(slash + 1);
            }

            if (!provider_name.empty() && provider_name != agent_->provider()->get_name()) {
                auto new_provider = ProviderRegistry::instance().create(provider_name);
                if (!new_provider) {
                    out("未知提供商: " + provider_name + "\n");
                    return;
                }
                agent_->set_provider(std::move(new_provider));
                out("提供商已设置为: " + provider_name + "\n");
            }

            ModelConfig mc;
            mc.model_name = model_name;
            mc.context_limit = resolve_context_limit(model_name);
            mc.reasoning_effort = Config::global().get_reasoning_effort();
            agent_->set_model_config(mc);

            out("模型已设置为: " + model_name + " (上下文 " +
                std::to_string(*mc.context_limit / 1000) + "k)\n");
        }
    } else if (cmd.command == "/prompt") {
        auto& cfg = Config::global();
        if (cmd.args.empty()) {
            std::string cur = cfg.get_system_prompt_template();
            out(std::string("当前系统提示词方案: ") +
                (cur == "tiny_model_system.md" ? "tiny（精简版，适合小模型）"
                                               : "system（完整版）") +
                "\n");
            out("用法: /prompt [system|tiny]\n");
        } else {
            std::string arg = cmd.args[0];
            if (arg == "system") {
                cfg.set_system_prompt_template("system.md");
                out("已切换为完整版系统提示词。\n");
            } else if (arg == "tiny") {
                cfg.set_system_prompt_template("tiny_model_system.md");
                out("已切换为精简版系统提示词（小模型方案）。\n");
            } else {
                out("未知方案: " + arg + "（可选: system / tiny）\n");
            }
        }
    } else if (cmd.command == "/compact") {
        if (messages_.len() <= 1) {
            out("没有可压缩的内容。\n");
            return;
        }
        auto compact_result = compact_messages(
            messages_, *agent_->provider(), agent_->model_config(), true);
        if (compact_result) {
            messages_ = compact_result->conversation;
            SessionManager::instance().replace_messages(session_id_, messages_.messages());
            refresh_context_tokens();
            out("对话已压缩。\n");
            if (tui_active_) {
                ui_->set_compact_result(build_compacted_text(messages_));
            }
        } else {
            out("压缩失败: " + compact_result.error().message + "\n");
        }
    } else if (cmd.command == "/history") {
        auto& session_mgr = SessionManager::instance();
        auto sessions = session_mgr.list_sessions();
        if (sessions) {
            out("会话列表:\n");
            for (const auto& s : *sessions) {
                out("  " + s.id.substr(0, 20) + "...  " + s.name + "  " +
                    utils::format_time(s.updated_at) + "\n");
            }
        }
    } else {
        out("未知命令: " + cmd.command + "\n");
    }
}

}} // namespace goose::cli
