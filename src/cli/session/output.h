#pragma once

#include <string>
#include <sstream>
#include "../../core/types.h"
#include "../../core/agent_event.h"

namespace goose {
namespace cli {

// 流式 markdown 渲染器：累积增量文本，按完整行渲染，并维护代码块等
// 跨行/跨增量边界的状态（如 LLM 流式输出的 `**粗` 与 `体**` 分属两个 delta）。
class MarkdownStream {
public:
    // 追加一段文本增量，返回已渲染的输出。
    std::string feed(const std::string& delta);
    // 冲刷未完成行与未闭合的代码块，返回已渲染的输出。
    std::string flush();

private:
    static constexpr size_t kMaxBufferedChars = 8192;
    std::string line_buffer_;
    bool in_code_block_ = false;
    std::string code_lang_;
};

// include_thinking=false 时跳过 ThinkingContent（思考已在流式中实时显示）。
void render_message(const Message& msg, bool include_thinking = true);
void render_tool_request(const ToolRequest& req);
void render_tool_response(const ToolResponse& resp);
void render_error(const std::string& error);
void render_agent_event(const AgentEvent& event);
std::string truncate_text(const std::string& text, size_t max_lines = 20);
std::string render_markdown(const std::string& text);

// 渲染输出目标流（TUI 模式注入聊天区；nullptr 恢复为 stdout）。
void set_render_stream(std::ostream* os);

}} // namespace goose::cli
