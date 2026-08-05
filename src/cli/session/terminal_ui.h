#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <cstdint>
#include <functional>
#include <ostream>
#include <spdlog/spdlog.h>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include "../../core/agent_event.h"

namespace goose {
namespace cli {

// 全屏 TUI：FTXUI 组件树驱动（顶栏 + 分块聊天区 + 底部输入行 + 工具确认 Modal）。
// 双线程：FTXUI Loop 跑在独立 UI 线程（接管键盘/鼠标/resize/绘制），
// 主线程保留阻塞的 read_line_input/confirm_tool 与 agent reply；
// 聊天区直接消费 agent 事件构造语义化分块模型（用户/思考/工具调用/回答），
// 用 FTXUI 原生 per-segment 样式渲染，不再经过 output.cpp 的 ANSI 流。
class TerminalUi {
public:
    TerminalUi();
    ~TerminalUi();

    TerminalUi(const TerminalUi&) = delete;
    TerminalUi& operator=(const TerminalUi&) = delete;

    // 进入全屏模式（FTXUI 自管理备用屏幕/termios/mouse）。非 TTY 时返回 false。
    bool enter();
    void exit();

    void set_session_info(const std::string& session_id, const std::string& name,
                          const std::string& provider, const std::string& model);
    void append_status_text(const std::string& text);

    void on_agent_event(const AgentEvent& event);

    void append_user_message(const std::string& text);
    void begin_assistant_turn();

    bool confirm_tool(const std::string& tool_name, const std::string& summary);

    void start_refresh();
    void stop_refresh();

    void set_compact_point();
    void set_compact_result(const std::string& text);

    // 标记 AI 是否正在工作。工作期间普通输入回车会被拦截提示，/ 命令走忙碌回调。
    void set_busy(bool busy);
    void set_busy_command_handler(std::function<void(const std::string&)> handler);
    bool is_busy() const { return busy_.load(); }

    // 中断工具确认模态（/stop 使用）。
    void interrupt_confirm();

    // 中断后复位顶栏工作状态（线程安全，/stop 后调用）。
    void notify_work_idle();

    // 阻塞读取一行输入（返回 "" 表示应退出，即 Ctrl-D 且空缓冲）。
    std::string read_line_input();

    void tick();

    enum class WorkState { Idle, Reasoning, Generating, Tool, Done };

    // ---- 聊天分块模型（公开供渲染 helper 使用）----
    struct StyledPiece {
        std::string text;
        bool bold = false;
        bool italic = false;
        bool dim = false;
        bool underline = false;
        ftxui::Color fg;
        bool has_fg = false;
    };
    using StyledRow = std::vector<StyledPiece>;

    enum class BlockKind { User, Status, Thinking, ToolCall, Reply, Error };

    struct MsgBlock {
        BlockKind kind = BlockKind::Status;
        std::string raw;                    // markdown 原文（thinking/reply）
        std::vector<StyledRow> rows;        // 已渲染的显示行（缓存）
        int rows_w = -1;                    // 缓存对应的内容列宽
        bool stream_open = false;           // 正在被流式追加
        bool expanded = false;              // 工具块输出展开
        std::string tool_name;
        std::string tool_args;
        std::string tool_output;            // 截断后的输出
        bool tool_has_output = false;
        std::string extra;                  // 可折叠块（如压缩结果）点击展开的内容
    };

private:
    friend struct MdRenderTestAccess;
    void close_streams();
    void render_md_block(MsgBlock& block, ftxui::Color base_color, int content_width);
    std::vector<StyledRow> render_md_rows(const std::string& raw,
                                          bool& in_code, std::string& lang,
                                          ftxui::Color base_color, int max_cols);
    static std::vector<StyledRow> wrap_row(const StyledRow& row, int width);
    static std::vector<StyledRow> tool_body_rows(const MsgBlock& block, int width);
    static std::vector<StyledRow> status_rows(const std::string& raw, int width);
    static void render_table(const std::vector<std::vector<std::string>>& cells,
                             const std::vector<int>& aligns,
                             int max_cols, ftxui::Color base_color,
                             std::vector<StyledRow>& out);

    // ---- 顶栏 / 布局 ----
    ftxui::Element topbar_element();
    ftxui::Element chat_element();
    ftxui::Element input_line_element();
    ftxui::Element confirm_modal_element_pure();

    // 滚动与工具点击命中
    void scroll_by(int delta);       // 正值=向上翻，0=滚回底部，-1=跳到顶部
    bool on_root_event(ftxui::Event event);
    bool on_confirm_event(ftxui::Event event);
    void confirm_resolve(bool result);

    void ui_main();
    void build_root();
    ftxui::Element layout_element();
    bool confirm_visible() const { return show_confirm_.load(); }

    // ---- 消息/状态入口 ----
    void append_status(const std::string& text);

    void update_usage(int64_t input, int64_t output);
    void set_work_state(WorkState state, const std::string& tool_name = "");
    void request_redraw();

    static const char* block_label(BlockKind kind);
    ftxui::Color block_color(BlockKind kind);

    // ---- UI 线程 ----
    ftxui::Component root_;
    ftxui::Component input_;
    // UI 线程在栈上创建 ScreenInteractive，成员保存裸指针供主线程触发重绘/退出。
    std::atomic<ftxui::ScreenInteractive*> screen_{nullptr};

    ftxui::InputOption input_option_;
    std::string input_buf_;
    int input_cursor_pos_ = 0;
    std::atomic<bool> show_confirm_{false};
    std::atomic<bool> input_placeholder_{false};

    // ---- 输入桥接（主线程阻塞 ← UI 线程 on_enter）----
    std::mutex input_cv_mutex_;
    std::condition_variable input_cv_;
    bool input_pending_ = false;
    bool input_submitted_ = false;
    std::string input_value_;

    // AI 工作期间的输入拦截与 / 命令路由。
    std::atomic<bool> busy_{false};
    std::function<void(const std::string&)> busy_command_handler_;

    void submit_input(const std::string& value);

    // ---- 确认弹窗桥接 ----
    std::mutex confirm_mutex_;
    std::condition_variable confirm_cv_;
    bool confirm_done_ = false;
    bool confirm_result_ = false;
    std::string confirm_tool_name_;
    std::string confirm_summary_;

    std::atomic<bool> active_{false};

    int chat_w_ = 80;
    int chat_h_ = 20;

    // 聊天分块缓冲
    std::vector<MsgBlock> blocks_;
    size_t off_top_ = 0;           // 从底部往上偏移的显示行数（0=跟随底部）
    static constexpr size_t kMaxBlocks = 2000;

    // 流式去重
    bool turn_streamed_reasoning_ = false;
    bool turn_streamed_text_ = false;
    int open_thinking_ = -1;
    int open_reply_ = -1;

    // 滚动与工具点击命中（chat_element 每次渲染时刷新，仅 UI 线程使用）
    struct ToolHit {
        int abs_row;       // 块起始显示行的全局索引
        int height;        // 块总行数（含边框）
        int block_index;   // tools_ 中的块索引
    };
    std::vector<ToolHit> tool_hits_;
    size_t render_begin_ = 0;     // 本次渲染可见首行（全局显示行索引）
    size_t total_display_rows_ = 0;
    std::string session_id_;
    std::string session_name_;
    std::string provider_name_;
    std::string model_name_;
    std::shared_ptr<spdlog::logger> saved_logger_;
    int64_t turn_input_ = 0;
    int64_t turn_output_ = 0;
    int64_t total_input_ = 0;
    int64_t total_output_ = 0;
    int64_t compact_point_tokens_ = 0;

    WorkState state_ = WorkState::Idle;
    std::string state_tool_;
    std::atomic<uint32_t> tick_count_{0};
    std::atomic<time_t> ui_last_heartbeat_{0};

    std::mutex model_mutex_;

    std::thread ui_thread_;
    std::thread refresh_thread_;
    std::atomic<bool> refresh_stop_{true};
    std::atomic<bool> ui_thread_alive_{false};

    void refresh_loop();

    std::thread::id ui_thread_id_{};
};

// 工具函数（供测试）
int utf8_char_width(uint32_t cp);
uint32_t utf8_decode_next(const std::string& s, size_t& i);
std::string truncate_by_width(const std::string& s, int max_cols);
std::string wrap_by_width(const std::string& s, int max_cols);
std::string format_token(int64_t tokens);
std::string pad_or_truncate_to_width(const std::string& s, int max_cols);

}} // namespace goose::cli