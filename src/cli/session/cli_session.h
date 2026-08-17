#pragma once

#include <string>
#include <memory>
#include "../../core/agent.h"
#include "../../core/types.h"
#include "input.h"
#include "terminal_ui.h"

namespace goose {
namespace cli {

class CliSession {
public:
    CliSession(std::unique_ptr<Agent> agent, const std::string& session_id);
    ~CliSession();

    CliSession(CliSession&&) noexcept;
    CliSession& operator=(CliSession&&) noexcept;

    int run_interactive();
    int run_headless(const std::string& prompt);

    void set_quiet(bool q) { quiet_ = q; }
    void set_max_turns(int m) { max_turns_ = m; }
    void set_resumed(bool r) { resumed_ = r; }
    void set_conversation(const Conversation& conv) { messages_ = conv; }
    void set_output_format(const std::string& f) { output_format_ = f; }

private:
    std::unique_ptr<Agent> agent_;
    std::string session_id_;
    Conversation messages_;
    bool quiet_ = false;
    int max_turns_ = 0;
    int turn_count_ = 0;
    bool resumed_ = false;
    std::string output_format_ = "text";

    Result<void> handle_agent_event(const AgentEvent& event);
    void handle_slash_command(const ParsedInput& cmd);
    void render_json_event(const AgentEvent& event);
    void maybe_auto_compact();
    // 将当前会话上下文 token 估算同步给顶部栏（TUI 激活时）。
    void refresh_context_tokens();
    bool should_exit_ = false;

    std::unique_ptr<TerminalUi> ui_;
    bool tui_active_ = false;
    // 输出路由：TUI 激活时命令/状态输出进入聊天区
    void out(const std::string& s);
};

}} // namespace goose::cli
