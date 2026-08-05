#include "terminal_ui.h"
#include "output.h"
#include "../../config/paths.h"
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/loop.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <unistd.h>
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <limits>
#include <cwchar>
#include <locale.h>
#include <ctime>
#include <filesystem>
#include <system_error>

namespace goose {
namespace cli {

// ============ 宽度/格式化 工具函数（供测试） ============

int utf8_char_width(uint32_t cp) {
    int w = wcwidth(static_cast<wchar_t>(cp));
    if (w >= 0) return w;
    // wcwidth 返回 -1（locale 未设 UTF-8 或未分配字符）：回退硬编码范围
    if (cp == 0 || cp < 0x20 || (cp >= 0x7f && cp < 0xa0)) return 0;
    bool wide =
        (cp >= 0x1100 && (cp <= 0x115f || cp == 0x2329 || cp == 0x232a ||
                          (cp >= 0x2e80 && cp <= 0xa4cf && cp != 0x303f) ||
                          (cp >= 0xac00 && cp <= 0xd7a3) ||
                          (cp >= 0xf900 && cp <= 0xfaff) ||
                          (cp >= 0xfe30 && cp <= 0xfe6f) ||
                          (cp >= 0xff01 && cp <= 0xff60) ||
                          (cp >= 0xffe0 && cp <= 0xffe6)));
    return wide ? 2 : 1;
}

uint32_t utf8_decode_next(const std::string& s, size_t& i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
        i++;
        return c;
    }
    int len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
    if (i + len > s.size()) {
        i++;
        return 0xFFFD;
    }
    uint32_t cp = c & (0xFF >> (len + 1));
    for (int k = 1; k < len; k++) {
        cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    }
    i += len;
    return cp;
}

int display_width(const std::string& s) {
    int w = 0;
    for (size_t i = 0; i < s.size();) {
        w += utf8_char_width(utf8_decode_next(s, i));
    }
    return w;
}

std::string utf8_encode_cp(char32_t cp) {
    if (cp <= 0x7F) return std::string(1, static_cast<char>(cp));
    if (cp <= 0x7FF) {
        return std::string({static_cast<char>(0xC0 | (cp >> 6)),
                            static_cast<char>(0x80 | (cp & 0x3F))});
    }
    if (cp <= 0xFFFF) {
        return std::string({static_cast<char>(0xE0 | (cp >> 12)),
                            static_cast<char>(0x80 | ((cp >> 6) & 0x3F)),
                            static_cast<char>(0x80 | (cp & 0x3F))});
    }
    return std::string({static_cast<char>(0xF0 | (cp >> 18)),
                        static_cast<char>(0x80 | ((cp >> 12) & 0x3F)),
                        static_cast<char>(0x80 | ((cp >> 6) & 0x3F)),
                        static_cast<char>(0x80 | (cp & 0x3F))});
}

std::string repeat_utf8(char32_t cp, int n) {
    std::string result;
    if (n <= 0) return result;
    std::string ch = utf8_encode_cp(cp);
    for (int i = 0; i < n; ++i) result += ch;
    return result;
}

std::string truncate_by_width(const std::string& s, int max_cols) {
    if (max_cols <= 0) return "";
    if (display_width(s) <= max_cols) return s;
    std::string result;
    int used = 0;
    size_t i = 0;
    while (i < s.size()) {
        size_t start = i;
        uint32_t cp = utf8_decode_next(s, i);
        int cw = utf8_char_width(cp);
        if (used + cw > max_cols - 1) break;
        result += s.substr(start, i - start);
        used += cw;
    }
    return result + "\u2026";
}

std::string pad_or_truncate_to_width(const std::string& s, int max_cols) {
    if (max_cols <= 0) return "";
    int w = display_width(s);
    if (w < max_cols) {
        return s + std::string(max_cols - w, ' ');
    }
    if (w == max_cols) return s;
    std::string result;
    int used = 0;
    size_t i = 0;
    while (i < s.size()) {
        size_t start = i;
        uint32_t cp = utf8_decode_next(s, i);
        int cw = utf8_char_width(cp);
        if (used + cw > max_cols) break;
        result += s.substr(start, i - start);
        used += cw;
    }
    return result;
}

// 渲染进度探针：UI 线程在关键阶段更新时间戳，refresh 线程据此判断卡死位置。
// 纯自旋循环里打 spdlog 会拖慢，这里只用 atomic 存储，开销极低。
static std::atomic<const char*> g_render_stage{"idle"};
static std::atomic<int64_t> g_render_stage_ns{0};

static void mark_stage(const char* stage) {
    g_render_stage.store(stage);
    g_render_stage_ns.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

static int64_t stage_age_ns() {
    int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return now - g_render_stage_ns.load();
}

// 生成 n 个 U+2500 ─ 字符（UTF-8：3 字节/个，显示宽度 1）。
static std::string dash_repeat(int n) {
    if (n <= 0) return "";
    std::string r;
    r.reserve(static_cast<size_t>(n) * 3);
    for (int i = 0; i < n; i++) r.append("\xe2\x94\x80", 3);
    return r;
}

// 自右向左保留 max_cols 宽，超长时头部插入省略号（顶栏 suffix 溢出用）。
static std::string truncate_leading_by_width(const std::string& s, int max_cols) {
    if (max_cols <= 0) return "";
    if (display_width(s) <= max_cols) return s;

    size_t cut = s.size();
    int used = 0;
    while (cut > 0) {
        size_t cstart = cut;
        while (cstart > 0 && (static_cast<unsigned char>(s[cstart - 1]) & 0xC0) == 0x80) {
            cstart--;
        }
        size_t dec = cstart;
        uint32_t cp = utf8_decode_next(s, dec);
        int cw = utf8_char_width(cp);
        if (used + cw > max_cols - 1) break;
        used += cw;
        cut = cstart;
    }
    if (cut == 0) return s;
    return "\u2026" + s.substr(cut);
}

// 按字符断行：宽度裁剪到 max_cols，超长部分作为下一行。
std::string wrap_by_width(const std::string& s, int max_cols) {
    if (max_cols <= 0) return s;
    if (display_width(s) <= max_cols) return s;

    std::string out;
    int used = 0;
    size_t i = 0;
    while (i < s.size()) {
        size_t start = i;
        uint32_t cp = utf8_decode_next(s, i);
        int cw = utf8_char_width(cp);
        if (used + cw > max_cols) {
            if (used == 0) {
                out += s.substr(start, i - start);
                used = cw;
                continue;
            }
            out += '\n';
            used = 0;
            i = start;
            continue;
        }
        out += s.substr(start, i - start);
        used += cw;
    }
    return out;
}

std::string format_token(int64_t tokens) {
    if (tokens < 0) tokens = 0;
    if (tokens < 1000) return std::to_string(tokens);
    char buf[32];
    if (tokens < 1000000) {
        snprintf(buf, sizeof(buf), "%.1fK", static_cast<double>(tokens) / 1000.0);
    } else {
        snprintf(buf, sizeof(buf), "%.1fM", static_cast<double>(tokens) / 1000000.0);
    }
    return buf;
}

static std::string strip_ansi(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\x1b') {
            if (i + 1 < s.size() && s[i + 1] == '[') {
                i += 2;
                while (i < s.size() && (s[i] < 0x40 || s[i] > 0x7e)) i++;
                if (i < s.size() && s[i] >= 0x40 && s[i] <= 0x7e) i++;
                else i--;
                continue;
            }
            if (i + 1 < s.size() && s[i + 1] == ']') {
                while (i < s.size() && s[i] != '\a' &&
                       !(i + 1 < s.size() && s[i] == '\x1b' && s[i + 1] == '\\')) {
                    i++;
                }
                if (i < s.size() && s[i] == '\a') i++;
                else if (i + 1 < s.size() && s[i] == '\x1b') i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

// ============ 富文本分段 ============

namespace {

struct MdStyle {
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool dim = false;
    ftxui::Color fg;
    bool has_fg = false;
};

bool same_style(const TerminalUi::StyledPiece& a, const MdStyle& b) {
    return a.bold == b.bold && a.italic == b.italic && a.underline == b.underline &&
           a.dim == b.dim && a.has_fg == b.has_fg && (!a.has_fg || a.fg == b.fg);
}

void push_glyph(TerminalUi::StyledRow& row, const std::string& text, const MdStyle& st) {
    if (!row.empty() && same_style(row.back(), st)) {
        row.back().text += text;
        return;
    }
    TerminalUi::StyledPiece p;
    p.text = text;
    p.bold = st.bold;
    p.italic = st.italic;
    p.underline = st.underline;
    p.dim = st.dim;
    p.has_fg = st.has_fg;
    p.fg = st.fg;
    row.push_back(std::move(p));
}

void push_spaces(TerminalUi::StyledRow& row, int n) {
    if (n <= 0) return;
    TerminalUi::StyledPiece p;
    p.text = std::string(n, ' ');
    row.insert(row.begin(), std::move(p));
}

bool is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

// 行内 markdown 扫描：**加粗**、*斜体*、`code`、[链接]（可互相嵌套）。
void inline_scan(const std::string& s, size_t begin, size_t end, MdStyle st,
                 TerminalUi::StyledRow& out) {
    MdStyle plain = st;
    size_t i = begin;
    std::string buf;
    auto flush = [&] {
        if (buf.empty()) return;
        push_glyph(out, buf, plain);
        buf.clear();
    };
    mark_stage("inline_scan");
    uint64_t loop_cnt = 0;
    while (i < end) {
        if ((++loop_cnt & 255) == 0) mark_stage("inline_scan");  // 高亮卡死在哪
        char c = s[i];
        if (c == '`') {
            size_t close = s.find('`', i + 1);
            if (close != std::string::npos && close < end) {
                flush();
                MdStyle code = plain;
                code.fg = ftxui::Color::Cyan;
                code.has_fg = true;
                push_glyph(out, s.substr(i + 1, close - i - 1), code);
                i = close + 1;
                continue;
            }
        }
        if ((c == '*' || c == '_') && i + 1 < end && s[i + 1] == c) {
            size_t close = s.find(std::string(2, c), i + 2);
            if (close != std::string::npos && close < end) {
                flush();
                MdStyle b = plain;
                b.bold = true;
                inline_scan(s, i + 2, close, b, out);
                i = close + 2;
                continue;
            }
        } else if (c == '*') {
            size_t close = s.find('*', i + 1);
            if (close != std::string::npos && close < end) {
                flush();
                MdStyle it = plain;
                it.italic = true;
                inline_scan(s, i + 1, close, it, out);
                i = close + 1;
                continue;
            }
        } else if (c == '_' && (i == 0 || !is_word_char(s[i - 1]))) {
            size_t close = s.find('_', i + 1);
            if (close != std::string::npos && close < end) {
                flush();
                MdStyle it = plain;
                it.italic = true;
                inline_scan(s, i + 1, close, it, out);
                i = close + 1;
                continue;
            }
        }
        if (c == '[') {
            size_t close_bracket = s.find(']', i + 1);
            if (close_bracket != std::string::npos && close_bracket < end &&
                close_bracket + 1 < end && s[close_bracket + 1] == '(') {
                size_t close_paren = s.find(')', close_bracket + 2);
                if (close_paren != std::string::npos && close_paren < end) {
                    flush();
                    MdStyle lk = plain;
                    lk.underline = true;
                    lk.fg = ftxui::Color::LightSkyBlue1;
                    lk.has_fg = true;
                    inline_scan(s, i + 1, close_bracket, lk, out);
                    i = close_paren + 1;
                    continue;
                }
            }
        }
        if (c == '\\' && i + 1 < end) {
            buf += s[i + 1];
            i += 2;
            continue;
        }
        size_t start = i;
        utf8_decode_next(s, i);
        buf += s.substr(start, i - start);
    }
    flush();
}

std::string ascii_trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) b++;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) e--;
    return s.substr(b, e - b);
}

std::vector<std::string> split_pipe_parts(const std::string& s) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= s.size()) {
        size_t p = s.find('|', start);
        if (p == std::string::npos) {
            parts.push_back(ascii_trim(s.substr(start)));
            break;
        }
        parts.push_back(ascii_trim(s.substr(start, p - start)));
        start = p + 1;
    }
    return parts;
}

// 分隔行，如 |:---|:--:|---:|
bool is_table_separator(const std::string& line) {
    std::string t = ascii_trim(line);
    if (t.find('|') == std::string::npos) return false;
    if (!t.empty() && t.front() == '|') t = t.substr(1);
    if (!t.empty() && t.back() == '|') t.pop_back();
    if (t.empty()) return false;
    bool any = false;
    for (const auto& part : split_pipe_parts(t)) {
        if (part.empty()) return false;
        size_t b = 0, e = part.size();
        if (part[b] == ':') b++;
        if (e > b && part[e - 1] == ':') e--;
        if (b >= e) return false;
        for (size_t k = b; k < e; k++) {
            if (part[k] != '-') return false;
        }
        any = true;
    }
    return any;
}

bool is_table_row(const std::string& line) {
    std::string t = ascii_trim(line);
    return !t.empty() && t.find('|') != std::string::npos;
}

std::vector<std::string> split_table_row(const std::string& line) {
    std::string t = ascii_trim(line);
    if (!t.empty() && t.front() == '|') t = t.substr(1);
    if (!t.empty() && t.back() == '|') t.pop_back();
    return split_pipe_parts(t);
}

// 0=左 1=中 2=右
std::vector<int> parse_table_aligns(const std::string& sep) {
    std::string t = ascii_trim(sep);
    if (!t.empty() && t.front() == '|') t = t.substr(1);
    if (!t.empty() && t.back() == '|') t.pop_back();
    std::vector<int> aligns;
    for (const auto& part : split_pipe_parts(t)) {
        if (part.empty()) {
            aligns.push_back(0);
            continue;
        }
        bool left = part.front() == ':';
        bool right = part.back() == ':';
        if (left && right) aligns.push_back(1);
        else if (right) aligns.push_back(2);
        else aligns.push_back(0);
    }
    return aligns;
}

int styled_row_width(const TerminalUi::StyledRow& row) {
    int w = 0;
    for (const auto& p : row) w += std::max(1, display_width(p.text));
    return w;
}

} // namespace

std::vector<TerminalUi::StyledRow> TerminalUi::wrap_row(const StyledRow& row, int width) {
    std::vector<StyledRow> rows;
    if (width <= 0) {
        rows.push_back(row);
        return rows;
    }
    rows.emplace_back();
    int used = 0;
    for (const auto& piece : row) {
        size_t i = 0;
        uint64_t loop_cnt = 0;
        while (i < piece.text.size()) {
            if ((++loop_cnt & 255) == 0) mark_stage("wrap_row");
            size_t start = i;
            uint32_t cp = utf8_decode_next(piece.text, i);
            int cw = std::max(1, utf8_char_width(cp));
            if (used > 0 && used + cw > width) {
                rows.emplace_back();
                used = 0;
            }
            MdStyle st;
            st.bold = piece.bold;
            st.italic = piece.italic;
            st.underline = piece.underline;
            st.dim = piece.dim;
            st.has_fg = piece.has_fg;
            st.fg = piece.fg;
            push_glyph(rows.back(), piece.text.substr(start, i - start), st);
            used += cw;
        }
    }
    return rows;
}

std::vector<TerminalUi::StyledRow> TerminalUi::render_md_rows(
    const std::string& raw, bool& in_code, std::string& code_lang,
    ftxui::Color base_color, int max_cols) {
    std::vector<StyledRow> out_rows;
    MdStyle base;
    base.fg = base_color;
    base.has_fg = true;

    std::vector<std::string> lines;
    std::istringstream iss(raw);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }

    size_t li = 0;
    uint64_t line_cnt = 0;
    while (li < lines.size()) {
        if ((++line_cnt & 255) == 0) mark_stage("render_md_rows");
        const std::string& ln = lines[li];
        StyledRow logical;

        if (in_code) {
            if (ln.rfind("```", 0) == 0) {
                in_code = false;
                code_lang.clear();
                MdStyle gutter = base;
                gutter.dim = true;
                gutter.fg = ftxui::Color::GrayLight;
                push_glyph(logical, "\u2514\u2500", gutter);
            } else {
                MdStyle code = base;
                code.dim = true;
                code.fg = ftxui::Color::GrayLight;
                push_glyph(logical, "\u2502 ", code);
                inline_scan(ln, 0, ln.size(), code, logical);
            }
            std::vector<StyledRow> wrapped = wrap_row(logical, max_cols);
            out_rows.insert(out_rows.end(), wrapped.begin(), wrapped.end());
            li++;
            continue;
        }

        if (ln.rfind("```", 0) == 0) {
            in_code = true;
            code_lang = ln.substr(3);
            MdStyle g = base;
            g.dim = true;
            g.fg = ftxui::Color::GrayLight;
            push_glyph(logical, "\u250c\u2500" + code_lang, g);
            std::vector<StyledRow> wrapped = wrap_row(logical, max_cols);
            out_rows.insert(out_rows.end(), wrapped.begin(), wrapped.end());
            li++;
            continue;
        }

        // 表格：当前行含 '|' 且下一行是分隔行
        if (li + 1 < lines.size() && is_table_separator(lines[li + 1]) &&
            is_table_row(ln)) {
            std::vector<std::vector<std::string>> cells;
            std::vector<int> aligns = parse_table_aligns(lines[li + 1]);
            cells.push_back(split_table_row(ln));
            li += 2;  // 跳过表头与分隔行
            while (li < lines.size() && is_table_row(lines[li]) &&
                   !is_table_separator(lines[li])) {
                cells.push_back(split_table_row(lines[li]));
                li++;
            }
            render_table(cells, aligns, max_cols, base_color, out_rows);
            continue;
        }

        if (ln.empty()) {
            push_glyph(logical, " ", base);
        } else {
            size_t content_start = 0;
            while (content_start < ln.size() &&
                   (ln[content_start] == ' ' || ln[content_start] == '\t')) {
                content_start++;
            }
            char first = content_start < ln.size() ? ln[content_start] : '\0';
            if (first == '#') {
                size_t hashes = 0;
                while (content_start + hashes < ln.size() && ln[content_start + hashes] == '#') {
                    hashes++;
                }
                std::string content = ln.substr(content_start + hashes);
                size_t ws = content.find_first_not_of(" \t");
                if (ws != std::string::npos) content = content.substr(ws);
                MdStyle h = base;
                h.bold = true;
                if (hashes <= 2) h.fg = ftxui::Color::Cyan;
                inline_scan(content, 0, content.size(), h, logical);
            } else if ((first == '-' || first == '*' || first == '+') ||
                       ln.compare(content_start, 1, "\u2022") == 0) {
                std::string rest = ln.substr(content_start);
                std::string indent = std::string(content_start, ' ');
                std::string prefix;
                std::string content;
                bool done = false;
                if (rest.compare(0, 3, "\u2022") == 0 && rest.size() > 3 &&
                    rest[3] == ' ') {
                    prefix = indent + "\u2022 ";
                    content = rest.substr(4);
                    done = true;
                } else if (rest.size() > 1 && rest[1] == ' ') {
                    if (rest.size() >= 6 && rest[2] == '[' &&
                        (rest[3] == ' ' || rest[3] == 'x' || rest[3] == 'X') &&
                        rest[4] == ']' && rest[5] == ' ') {
                        prefix = indent + (rest[3] == ' ' ? "\u2610 " : "\u2611 ");
                        content = rest.substr(6);
                        done = true;
                    } else {
                        prefix = indent + "\u2022 ";
                        content = rest.substr(2);
                        done = true;
                    }
                } else if (rest[0] >= '0' && rest[0] <= '9') {
                    size_t p = rest.find_first_of(".)");
                    if (p != std::string::npos && p > 0 && p + 1 < rest.size() &&
                        rest[p + 1] == ' ') {
                        prefix = indent + rest.substr(0, p + 2);
                        content = rest.substr(p + 2);
                        done = true;
                    }
                }
                if (!done) {
                    inline_scan(ln, 0, ln.size(), base, logical);
                } else {
                    MdStyle bullet = base;
                    bullet.fg = ftxui::Color::Green;
                    push_glyph(logical, prefix, bullet);
                    inline_scan(content, 0, content.size(), base, logical);
                }
            } else if (first == '>') {
                size_t q = content_start;
                while (q < ln.size() && ln[q] == '>') q++;
                MdStyle qs = base;
                qs.dim = true;
                qs.fg = ftxui::Color::GrayLight;
                push_glyph(logical, std::string(q - content_start - 1, ' ') + "\u2502 ", qs);
                size_t skip = q;
                if (skip < ln.size() && ln[skip] == ' ') skip++;
                inline_scan(ln.substr(skip), 0, ln.size() - skip, qs, logical);
            } else {
                bool hr = true;
                for (char ch : ln) {
                    if (ch != '-' && ch != '_' && ch != '*' && ch != ' ') {
                        hr = false;
                        break;
                    }
                }
                if (hr && ln.find_first_of("-_*") != std::string::npos) {
                    MdStyle r = base;
                    r.dim = true;
                    push_glyph(logical, dash_repeat(12), r);
                } else {
                    inline_scan(ln, 0, ln.size(), base, logical);
                }
            }
        }

        std::vector<StyledRow> wrapped = wrap_row(logical, max_cols);
        out_rows.insert(out_rows.end(), wrapped.begin(), wrapped.end());
        li++;
    }
    return out_rows;
}

void TerminalUi::render_table(const std::vector<std::vector<std::string>>& cells,
                              const std::vector<int>& aligns, int max_cols,
                              ftxui::Color base_color,
                              std::vector<StyledRow>& out) {
    if (cells.empty()) return;
    int ncols = 0;
    for (const auto& row : cells) ncols = std::max(ncols, static_cast<int>(row.size()));
    if (ncols == 0) return;

    std::vector<int> colw(ncols, 0);
    for (const auto& row : cells) {
        for (int c = 0; c < ncols && c < static_cast<int>(row.size()); c++) {
            colw[c] = std::max(colw[c], display_width(ascii_trim(row[c])));
        }
    }
    int total = ncols + 1;
    for (int c = 0; c < ncols; c++) total += colw[c] + 2;
    if (total > max_cols) {
        int excess = total - max_cols;
        while (excess > 0) {
            bool reduced = false;
            for (int c = 0; c < ncols && excess > 0; c++) {
                if (colw[c] > 1) {
                    colw[c]--;
                    excess--;
                    reduced = true;
                }
            }
            if (!reduced) break;
        }
    }

    MdStyle border;
    border.fg = base_color;
    border.has_fg = true;
    border.dim = true;

    auto add_border = [&](const std::string& l, const std::string& mid,
                          const std::string& r) {
        StyledRow br;
        push_glyph(br, l, border);
        for (int c = 0; c < ncols; c++) {
            push_glyph(br, repeat_utf8(U'\u2500', colw[c] + 2), border);
            push_glyph(br, (c == ncols - 1) ? r : mid, border);
        }
        out.push_back(br);
    };

    add_border("\u256d", "\u252c", "\u256e");
    for (int r = 0; r < static_cast<int>(cells.size()); r++) {
        std::vector<std::vector<StyledRow>> cell_rows(ncols);
        int maxh = 1;
        for (int c = 0; c < ncols; c++) {
            std::string txt = c < static_cast<int>(cells[r].size())
                                  ? ascii_trim(cells[r][c])
                                  : "";
            if (txt.empty()) {
                cell_rows[c].push_back(StyledRow{});
                continue;
            }
            MdStyle cell;
            cell.fg = base_color;
            cell.has_fg = true;
            if (r == 0) cell.bold = true;
            StyledRow raw_row;
            inline_scan(txt, 0, txt.size(), cell, raw_row);
            cell_rows[c] = wrap_row(raw_row, colw[c]);
            if (cell_rows[c].empty()) cell_rows[c].push_back(StyledRow{});
            maxh = std::max(maxh, static_cast<int>(cell_rows[c].size()));
        }
        for (int h = 0; h < maxh; h++) {
            StyledRow lr;
            push_glyph(lr, "\u2502", border);
            for (int c = 0; c < ncols; c++) {
                push_glyph(lr, " ", border);
                StyledRow content;
                if (h < static_cast<int>(cell_rows[c].size())) content = cell_rows[c][h];
                int w = styled_row_width(content);
                int pad = colw[c] - w;
                int left = 0, right = 0;
                int al = c < static_cast<int>(aligns.size()) ? aligns[c] : 0;
                if (al == 2) {
                    left = pad;
                } else if (al == 1) {
                    left = pad / 2;
                    right = pad - pad / 2;
                } else {
                    right = pad;
                }
                if (left > 0) push_glyph(lr, std::string(left, ' '), border);
                for (const auto& p : content) {
                    MdStyle st;
                    st.bold = p.bold;
                    st.italic = p.italic;
                    st.underline = p.underline;
                    st.dim = p.dim;
                    st.has_fg = p.has_fg;
                    st.fg = p.fg;
                    push_glyph(lr, p.text, st);
                }
                if (right > 0) push_glyph(lr, std::string(right, ' '), border);
                push_glyph(lr, " ", border);
                push_glyph(lr, "\u2502", border);
            }
            out.push_back(lr);
        }
        if (r == 0 && static_cast<int>(cells.size()) > 1) {
            add_border("\u251c", "\u253c", "\u2524");
        }
    }
    add_border("\u2570", "\u2534", "\u256f");
}

// ============ TerminalUi ============

TerminalUi::TerminalUi() {}

TerminalUi::~TerminalUi() {
    exit();
}

bool TerminalUi::enter() {
    if (active_) return true;
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return false;

    saved_logger_ = spdlog::default_logger();
    try {
        auto log_path = paths::log_dir() / "kacli.log";
        std::filesystem::create_directories(log_path.parent_path());
        auto file_logger = spdlog::basic_logger_mt("kacli-tui", log_path.string());
        // 跟随全局级别：未加 --debug 时请求体（可能含密钥）不落盘。
        file_logger->set_level(spdlog::get_level());
        // 每条日志立即落盘：卡死时能看到 refresh_loop 是否还在输出，
        // 避免 kill -9 导致缓冲里的哨兵日志丢失。
        file_logger->flush_on(spdlog::get_level());
        spdlog::set_default_logger(file_logger);
#ifndef _WIN32
        // 日志可能含对话内容，收紧到仅属主可读写。
        std::error_code ec;
        std::filesystem::permissions(
            log_path,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace, ec);
#endif
    } catch (const std::exception& e) {
        spdlog::warn("Unable to redirect TUI log to file: {}", e.what());
    }

    // 确保 locale 初始化，使 wcwidth 与 FTXUI 一致
    setlocale(LC_CTYPE, "");

    active_ = true;
    ui_thread_ = std::thread([this] { ui_main(); });
    return true;
}

void TerminalUi::exit() {
    if (!active_) return;
    stop_refresh();

    ftxui::ScreenInteractive* s = screen_.load();
    if (s != nullptr) {
        s->ExitLoopClosure()();
    }

    if (ui_thread_.joinable()) {
        ui_thread_.join();
    }
    screen_.store(nullptr);

    active_ = false;
    if (saved_logger_) {
        spdlog::set_default_logger(saved_logger_);
        saved_logger_.reset();
    }
}

void TerminalUi::request_redraw() {
    ftxui::ScreenInteractive* s = screen_.load();
    if (s != nullptr && active_) {
        s->PostEvent(ftxui::Event::Custom);
    }
}

// ============ UI 线程主体 ============

void TerminalUi::ui_main() {
    ftxui::ScreenInteractive screen = ftxui::ScreenInteractive::Fullscreen();
    screen.TrackMouse(true);
    screen_.store(&screen);
    ui_thread_id_ = std::this_thread::get_id();

    build_root();

    ftxui::Loop loop(&screen, root_);
    loop.Run();

    root_.reset();
    input_.reset();
    screen_.store(nullptr);
}

void TerminalUi::build_root() {
    input_option_ = ftxui::InputOption{};
    input_option_.multiline = false;
    input_option_.insert = true;
    input_option_.content = ftxui::StringRef(&input_buf_);
    input_option_.cursor_position = &input_cursor_pos_;
    input_option_.on_enter = [this] { submit_input(input_buf_); };
    // placeholder 留空：让 Input 的光标定位在输入起点（零宽文本）。
    // 灰色提示文案由 input_line_element 手动渲染。
    input_option_.placeholder = "";
    input_option_.transform = [this](ftxui::InputState state) {
        input_placeholder_.store(state.is_placeholder);
        if (state.is_placeholder) {
            // 零宽锚点：1 格宽，光标在输入起点。
            // 灰色提示文案由 input_line_element 在锚点后渲染。
            return state.focused ? (ftxui::text(" ") | ftxui::focusCursorBarBlinking)
                                 : (ftxui::text(" ") | ftxui::dim);
        }
        // 按光标字节位置切三段「before | 光标 bar | after」，光标 bar 落在
        // 字符边界（双宽中文时在其字符前，而非内部 x_max 的右半格）。
        const size_t pos =
            std::min<size_t>(static_cast<size_t>(input_cursor_pos_), input_buf_.size());
        ftxui::Element bar = state.focused
                                 ? (ftxui::text(" ") | ftxui::focusCursorBarBlinking)
                                 : ftxui::text(" ");
        ftxui::Element content = ftxui::hbox({
            ftxui::text(input_buf_.substr(0, pos)),
            bar,
            ftxui::text(input_buf_.substr(pos)),
        });
        return content | ftxui::color(ftxui::Color::White);
    };

    input_ = ftxui::Input(input_option_);

    auto main = ftxui::Renderer(input_, [this] {
        try {
            if (show_confirm_.load()) {
                return ftxui::dbox({
                    layout_element(),
                    ftxui::center(confirm_modal_element_pure()),
                });
            }
            return layout_element();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "FATAL: render crash: %s\n", e.what());
            return ftxui::text("渲染错误");
        } catch (...) {
            std::fprintf(stderr, "FATAL: unknown render crash\n");
            return ftxui::text("未知渲染错误");
        }
    });

    struct ScrollFirst : ftxui::ComponentBase {
        TerminalUi* ui;
        ftxui::Component inner;
        bool OnEvent(ftxui::Event e) override {
            if (!ui->confirm_visible() &&
                (e == ftxui::Event::Home || e == ftxui::Event::End)) {
                if (ui->on_root_event(e)) return true;
            }
            return inner->OnEvent(e);
        }
        ftxui::Element OnRender() override { return inner->Render(); }
    };
    auto scroll_first = std::make_shared<ScrollFirst>();
    scroll_first->ui = this;
    scroll_first->inner = main;
    root_ = ftxui::CatchEvent(scroll_first, [this](ftxui::Event event) {
        if (show_confirm_.load()) {
            return on_confirm_event(event);
        }
        return on_root_event(event);
    });
}

// ============ 布局 ============

ftxui::Element TerminalUi::layout_element() {
    mark_stage("layout_element:start");
    ui_last_heartbeat_.store(std::time(nullptr));
    ftxui::ScreenInteractive* s = screen_.load();
    int width = s != nullptr ? s->dimx() : 80;
    int height = s != nullptr ? s->dimy() : 24;
    chat_w_ = std::max(20, width);
    chat_h_ = std::max(3, height - 4);
    spdlog::debug("layout: dimx={} chat_w_={} chat_h_={}", width, chat_w_, chat_h_);

    ftxui::Elements doc;
    doc.push_back(topbar_element());
    doc.push_back(ftxui::separator());
    doc.push_back(chat_element() | ftxui::yflex);
    mark_stage("layout_element:after_chat");
    doc.push_back(ftxui::separator());
    doc.push_back(input_line_element());
    mark_stage("layout_element:done");
    return ftxui::vbox(std::move(doc));
}

const char* TerminalUi::block_label(BlockKind kind) {
    switch (kind) {
        case BlockKind::User: return "\u7528\u6237";
        case BlockKind::Thinking: return "\u601d\u8003";
        case BlockKind::ToolCall: return "\u5de5\u5177\u8c03\u7528";
        case BlockKind::Reply: return "\u56de\u7b54";
        case BlockKind::Error: return "\u9519\u8bef";
        case BlockKind::Status: return "";
    }
    return "";
}

ftxui::Color TerminalUi::block_color(BlockKind kind) {
    switch (kind) {
        case BlockKind::User: return ftxui::Color::White;
        case BlockKind::Thinking: return ftxui::Color::GrayLight;
        case BlockKind::ToolCall: return ftxui::Color::LightSkyBlue1;
        case BlockKind::Reply: return ftxui::Color::Yellow;
        case BlockKind::Error: return ftxui::Color::Red;
        case BlockKind::Status: return ftxui::Color::GrayDark;
    }
    return ftxui::Color::GrayDark;
}

// 各块的顶/底色条与内容的缩进。


ftxui::Element TerminalUi::topbar_element() {
    static const char* spin[] = {"/", "-", "\\", "|"};
    uint32_t tick = tick_count_.load();
    std::string state_plain;
    ftxui::Color state_color = ftxui::Color::GrayDark;

    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        if (state_ == WorkState::Tool) {
            state_plain = "工作中 " + std::string(spin[tick % 4]);
            state_color = ftxui::Color::Yellow;
        } else {
            switch (state_) {
                case WorkState::Reasoning:
                    state_plain = "思考中 " + std::string(spin[tick % 4]);
                    state_color = ftxui::Color::LightSkyBlue1;
                    break;
                case WorkState::Generating:
                    state_plain = "生成中 " + std::string(spin[tick % 4]);
                    state_color = ftxui::Color::Yellow;
                    break;
                case WorkState::Done:
                    state_plain = "完成";
                    state_color = ftxui::Color::Green;
                    break;
                case WorkState::Idle:
                default:
                    state_plain = "\u5f85\u8f93\u5165" + std::string((tick / 3) % 4, '.');
                    state_color = ftxui::Color::GrayDark;
                    break;
            }
        }

        constexpr int kStateBudget = 9;
        state_plain = pad_or_truncate_to_width(state_plain, kStateBudget);

        int64_t big = total_input_ + total_output_;
        int64_t small = big - compact_point_tokens_;
        if (small < 0) small = 0;

        std::string id5 = session_id_.size() >= 5 ? session_id_.substr(0, 5) : session_id_;
        std::string prefix = "\u250c\u2500 Kevin314 Agent Cli \u2502 " + model_name_ + " \u2502 ";
        std::string suffix = " \u2502  " + format_token(small) + "/" + format_token(big) +
                             "  \u2502  " + id5 + "  \u2500\u2510";

        int avail = std::max(1, chat_w_);
        int pw = display_width(prefix);
        int mw = display_width(state_plain);
        int sw = display_width(suffix);
        if (pw + mw + sw > avail) {
            if (pw + mw >= avail) {
                prefix = truncate_by_width(prefix, std::max(0, avail - mw));
            } else {
                suffix = truncate_leading_by_width(suffix, avail - pw - mw);
            }
        }

        return ftxui::hbox({
            ftxui::filler(),
            ftxui::text(prefix),
            ftxui::text(state_plain) | ftxui::color(state_color),
            ftxui::text(suffix),
            ftxui::filler(),
        });
    }
}

// 工具块的内容行：默认一行(名称+参数)，展开时追加输出。
std::vector<TerminalUi::StyledRow> TerminalUi::tool_body_rows(const MsgBlock& block, int width) {
    std::vector<StyledRow> rows;
    StyledRow header;
    MdStyle s;
    s.fg = ftxui::Color::LightSkyBlue1;
    s.has_fg = true;
    s.bold = true;
    push_glyph(header, "\u5de5\u5177\u8c03\u7528: ", s);  // 工具调用:
    s.bold = false;
    push_glyph(header, block.tool_name, s);
    if (!block.tool_args.empty()) {
        push_glyph(header, std::string(" ") + block.tool_args, s);
    }
    std::vector<StyledRow> wrapped = wrap_row(header, width);
    rows.insert(rows.end(), wrapped.begin(), wrapped.end());

    if (block.expanded && block.tool_has_output) {
        MdStyle dim;
        dim.dim = true;
        dim.fg = ftxui::Color::GrayLight;
        std::istringstream iss(block.tool_output);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.empty()) continue;
            mark_stage("tool_body_rows(output)");
            StyledRow r;
            push_spaces(r, 2);
            inline_scan(line, 0, line.size(), dim, r);
            auto wrapped2 = wrap_row(r, width);
            rows.insert(rows.end(), wrapped2.begin(), wrapped2.end());
        }
    }
    return rows;
}

std::vector<TerminalUi::StyledRow> TerminalUi::status_rows(const std::string& raw, int width) {
    MdStyle st;
    st.dim = true;
    st.fg = ftxui::Color::GrayDark;
    st.has_fg = true;

    std::vector<StyledRow> rows;
    size_t start = 0;
    while (start <= raw.size()) {
        size_t nl = raw.find('\n', start);
        std::string line = (nl == std::string::npos) ? raw.substr(start)
                                                     : raw.substr(start, nl - start);
        StyledRow r;
        inline_scan(line, 0, line.size(), st, r);
        auto wrapped = wrap_row(r, width);
        rows.insert(rows.end(), wrapped.begin(), wrapped.end());
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return rows;
}

ftxui::Element TerminalUi::chat_element() {
    mark_stage("chat_element:lock");
    std::lock_guard<std::mutex> lock(model_mutex_);
    mark_stage("chat_element:locked");
    spdlog::debug("chat_element: enter blocks={}", blocks_.size());

    const int bw = std::max(1, chat_w_ - 3);
    tool_hits_.clear();

    auto content_width_of = [bw](int indent) {
        return std::max(1, bw - indent);
    };
    const int line_w = bw + 2;
    spdlog::debug("chat_element: bw={} line_w={} chat_w_={}", bw, line_w, chat_w_);
    (void)line_w;

    // 计算每块视口行数（内容行 + 2 边框）
    spdlog::debug("chat_element: start loop blocks={}", blocks_.size());
    auto t0 = std::chrono::steady_clock::now();
    struct BlockInfo {
        int content_rows;
        int bi;
        BlockKind kind;
    };
    std::vector<BlockInfo> layout;
    int total = 0;

    for (int bi = 0; bi < static_cast<int>(blocks_.size()); bi++) {
        MsgBlock& block = blocks_[bi];
        int cr = 0;
        mark_stage("chat_element:compute");

        if (block.kind == BlockKind::ToolCall) {
            int ind = 2;
            int cw = content_width_of(ind);
            auto body = tool_body_rows(block, cw);
            cr = static_cast<int>(body.size());
        } else if (block.kind == BlockKind::Thinking || block.kind == BlockKind::Reply ||
                   block.kind == BlockKind::User || block.kind == BlockKind::Error) {
            auto t1 = std::chrono::steady_clock::now();
            render_md_block(block, block_color(block.kind), content_width_of(2));
            auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t1).count();
            if (dt > 50) {
                spdlog::warn("chat_element: slow render_md_block idx={} kind={} raw={}ms", bi, static_cast<int>(block.kind), dt);
            }
            cr = static_cast<int>(block.rows.size());
            if (cr == 0 && block.raw.empty()) cr = 1;
        } else {  // Status
            cr = static_cast<int>(status_rows(block.raw, content_width_of(2)).size());
            if (block.expanded && !block.extra.empty()) {
                cr += static_cast<int>(status_rows(block.extra, content_width_of(2)).size());
            }
        }

        if (cr == 0) cr = 1;
        layout.push_back({cr, bi, block.kind});
        total += cr + 2;
    }

    spdlog::debug("chat_element: loop done {} blocks in {}ms", layout.size(),
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count());

    total_display_rows_ = static_cast<size_t>(total);
    size_t maxoff = total > chat_h_ ? static_cast<size_t>(total - chat_h_) : 0;
    if (off_top_ > maxoff) off_top_ = maxoff;

    // 找到视口内第一个块
    size_t skip_rows = total > chat_h_ ? total - chat_h_ - off_top_ : 0;
    spdlog::debug("chat_element blocks={} total_rows={} skip_rows={}", layout.size(), total, skip_rows);
    size_t visible_max = skip_rows + static_cast<size_t>(chat_h_);
    size_t cum = 0;

    ftxui::Elements lines;
    size_t lines_start = 0;
    bool have_lines = false;
    for (auto& blk : layout) {
        size_t blk_total = static_cast<size_t>(blk.content_rows) + 2;
        size_t end = cum + blk_total;
        bool visible = end > skip_rows && cum < visible_max;
        size_t blk_start = cum;
        cum = end;
        if (!visible) continue;
        if (!have_lines) {
            lines_start = blk_start;
            have_lines = true;
        }

        MsgBlock& block = blocks_[blk.bi];
        ftxui::Color bc = block_color(blk.kind);

        // 获取内容行（md 块走缓存）
        std::vector<StyledRow> rows;
        if (blk.kind == BlockKind::ToolCall) {
            rows = tool_body_rows(block, content_width_of(2));
        } else if (blk.kind == BlockKind::Thinking || blk.kind == BlockKind::Reply ||
                   blk.kind == BlockKind::User || blk.kind == BlockKind::Error) {
            render_md_block(block, bc, content_width_of(2));
            rows = block.rows;
        } else {  // Status
            rows = status_rows(block.raw, content_width_of(2));
            if (block.expanded && !block.extra.empty()) {
                auto extra = status_rows(block.extra, content_width_of(2));
                rows.insert(rows.end(), extra.begin(), extra.end());
            }
        }

        if (rows.empty()) {
            rows.push_back({});
        }

        // 标题行（上边框）
        std::string title = block_label(blk.kind);
        if (blk.kind == BlockKind::Status && !block.extra.empty()) {
            title = block.expanded ? "\u538b\u7f29\u7ed3\u679c \u25bc (\u70b9\u51fb\u6298\u53e0)"
                                   : "\u538b\u7f29\u7ed3\u679c \u25b6 (\u70b9\u51fb\u5c55\u5f00)";
        }
        int title_w = display_width(title);
        std::string hfill = repeat_utf8(U'\u2500', std::max(0, chat_w_ - 2 - title_w));
        lines.push_back(
            ftxui::text("\u256d" + title + hfill + "\u256e") |
            ftxui::color(bc));

        // 内容行
        for (const auto& row : rows) {
            ftxui::Elements parts;
            parts.push_back(ftxui::text("\u2502  ") | ftxui::color(bc));
            for (const auto& piece : row) {
                auto e = ftxui::text(piece.text);
                if (piece.bold) e |= ftxui::bold;
                if (piece.italic) e |= ftxui::italic;
                if (piece.underline) e |= ftxui::underlined;
                if (piece.dim) e |= ftxui::dim;
                if (piece.has_fg) e |= ftxui::color(piece.fg);
                parts.push_back(std::move(e));
            }
            parts.push_back(ftxui::filler());
            parts.push_back(ftxui::text("\u2502") | ftxui::color(bc));
            lines.push_back(ftxui::hbox(std::move(parts)));
        }

        // 下边框
        std::string bfill = repeat_utf8(U'\u2500', std::max(0, chat_w_ - 2));
        lines.push_back(
            ftxui::text("\u2570" + bfill + "\u256f") |
            ftxui::color(bc));

        // 工具点击：记录块起始绝对行与高度
        if (blk.kind == BlockKind::ToolCall ||
            (blk.kind == BlockKind::Status && !block.extra.empty())) {
            tool_hits_.push_back({static_cast<int>(blk_start),
                                  static_cast<int>(blk_total),
                                  blk.bi});
        }
    }

    render_begin_ = skip_rows;
    size_t n = lines.size();
    if (n == 0) return ftxui::text("");
    size_t lo = skip_rows > lines_start ? skip_rows - lines_start : 0;
    size_t hi = std::min(lo + static_cast<size_t>(chat_h_), n);
    ftxui::Elements sel;
    sel.reserve(hi - lo);
    for (size_t i = lo; i < hi; ++i) sel.push_back(std::move(lines[i]));
    return ftxui::vbox(std::move(sel));
}

void TerminalUi::render_md_block(MsgBlock& block, ftxui::Color base_color, int content_width) {
    int bw = std::max(1, content_width);
    if (block.stream_open || block.rows_w != bw || block.rows.empty()) {
        bool in_code = false;
        std::string lang;
        block.rows = render_md_rows(block.raw, in_code, lang, base_color, bw);
        block.rows_w = bw;
    }
}

// ============ 输入行 ============

ftxui::Element TerminalUi::input_line_element() {
    std::vector<ftxui::Element> parts;
    parts.push_back(ftxui::text("  "));
    parts.push_back(ftxui::text("> ") | ftxui::color(ftxui::Color::GrayDark));
    auto input_el = input_->Render();
    if (input_placeholder_.load()) {
        // 输入起点锚点（transform 已换成 1 格宽），灰色提示在锚点后。
        parts.push_back(input_el);
        parts.push_back(ftxui::text("输入消息，Enter 发送，/help 查看命令") |
            ftxui::dim);
    } else {
        // Input 聚焦时内部已用 focusCursorBarBlinking 绘制光标，
        // 位置由 cursor_box_ 精确定位（跟随光标，含中文间）。
        parts.push_back(input_el);
    }
    parts.push_back(ftxui::text("  "));
    if (busy_.load()) {
        parts.push_back(ftxui::text("AI 正在工作，输入 /stop 中断") |
            ftxui::color(ftxui::Color::GrayDark));
    }
    parts.push_back(ftxui::filler());
    return ftxui::hbox(std::move(parts));
}

ftxui::Element TerminalUi::confirm_modal_element_pure() {
    std::string tool_name;
    std::string summary;
    {
        std::lock_guard<std::mutex> lock(confirm_mutex_);
        tool_name = confirm_tool_name_;
        summary = confirm_summary_;
    }
    int w = std::max(12, chat_w_ - 8);
    return ftxui::border(ftxui::window(
        ftxui::hbox({ftxui::text("\u26a0 "), ftxui::text("\u5de5\u5177\u786e\u8ba4")}) |
            ftxui::color(ftxui::Color::Yellow),
        ftxui::vbox({
            ftxui::text(truncate_by_width(strip_ansi(tool_name), w)),
            ftxui::text(truncate_by_width(strip_ansi(summary), w)),
            ftxui::text(""),
            ftxui::hbox({
                ftxui::text("[y] \u540c\u610f  "),
                ftxui::text("[n] \u62d2\u7edd  "),
                ftxui::text("Esc \u53d6\u6d88"),
                ftxui::filler(),
            }),
        })));
}

// ============ 事件处理 ============

bool TerminalUi::on_root_event(ftxui::Event e) {
    if (e == ftxui::Event::PageUp) {
        scroll_by(static_cast<int>((chat_h_ + 1) / 2));
        return true;
    }
    if (e == ftxui::Event::PageDown) {
        scroll_by(-static_cast<int>((chat_h_ + 1) / 2));
        return true;
    }
    if (e == ftxui::Event::Home) {
        std::lock_guard<std::mutex> lock(model_mutex_);
        size_t maxoff = total_display_rows_ > static_cast<size_t>(chat_h_)
                            ? total_display_rows_ - chat_h_
                            : 0;
        off_top_ = maxoff;
        request_redraw();
        return true;
    }
    if (e == ftxui::Event::End) {
        std::lock_guard<std::mutex> lock(model_mutex_);
        off_top_ = 0;
        request_redraw();
        return true;
    }
    if (e.is_mouse()) {
        auto m = e.mouse();
        if (m.button == ftxui::Mouse::WheelUp || m.button == ftxui::Mouse::WheelDown) {
            int step = static_cast<int>((chat_h_ + 3) / 4);
            scroll_by(m.button == ftxui::Mouse::WheelUp ? step : -step);
            return true;
        }
        if (m.button == ftxui::Mouse::Left && m.motion == ftxui::Mouse::Released) {
            int sy = m.y - 2;  // 顶栏(1) + 分隔(1)
            if (sy >= 0 && sy < chat_h_) {
                size_t abs = render_begin_ + static_cast<size_t>(sy);
                for (const auto& hit : tool_hits_) {
                    if (abs >= static_cast<size_t>(hit.abs_row) &&
                        abs < static_cast<size_t>(hit.abs_row + hit.height)) {
                        std::lock_guard<std::mutex> lock(model_mutex_);
                        if (static_cast<size_t>(hit.block_index) < blocks_.size()) {
                            auto& target = blocks_[hit.block_index];
                            if (target.kind == BlockKind::ToolCall ||
                                (target.kind == BlockKind::Status && !target.extra.empty())) {
                                target.expanded = !target.expanded;
                                request_redraw();
                                return true;
                            }
                        }
                    }
                }
            }
        }
    }
    return false;
}

void TerminalUi::scroll_by(int delta) {
    std::lock_guard<std::mutex> lock(model_mutex_);
    size_t maxoff = total_display_rows_ > static_cast<size_t>(chat_h_)
                        ? total_display_rows_ - chat_h_
                        : 0;
    if (delta > 0) {
        off_top_ = std::min(maxoff, off_top_ + static_cast<size_t>(delta));
    } else if (delta < 0) {
        off_top_ = off_top_ > static_cast<size_t>(-delta) ? off_top_ - static_cast<size_t>(-delta) : 0;
    } else {
        off_top_ = 0;
    }
    request_redraw();
}

bool TerminalUi::on_confirm_event(ftxui::Event e) {
    if (e == ftxui::Event::Character('y') || e == ftxui::Event::Character('Y')) {
        confirm_resolve(true);
        return true;
    }
    if (e == ftxui::Event::Character('n') || e == ftxui::Event::Character('N')) {
        confirm_resolve(false);
        return true;
    }
    if (e == ftxui::Event::Escape) {
        confirm_resolve(false);
        return true;
    }
    return true;
}

void TerminalUi::confirm_resolve(bool result) {
    std::lock_guard<std::mutex> lock(confirm_mutex_);
    confirm_result_ = result;
    confirm_done_ = true;
    show_confirm_.store(false);
    confirm_cv_.notify_one();
}

bool TerminalUi::confirm_tool(const std::string& tool_name, const std::string& summary) {
    {
        std::lock_guard<std::mutex> lock(confirm_mutex_);
        confirm_tool_name_ = tool_name;
        confirm_summary_ = summary;
        confirm_done_ = false;
    }
    show_confirm_.store(true);
    request_redraw();

    std::unique_lock<std::mutex> lock(confirm_mutex_);
    while (!confirm_cv_.wait_for(lock, std::chrono::seconds(2),
                                 [this] { return confirm_done_; })) {
        time_t hb = ui_last_heartbeat_.load();
        if (hb > 0 && std::time(nullptr) - hb > 10) {
            show_confirm_.store(false);
            return false;
        }
    }
    return confirm_result_;
}

// ============ 输入桥接 ============

void TerminalUi::submit_input(const std::string& value) {
    std::function<void(const std::string&)> handler;
    std::string command;
    {
        std::unique_lock<std::mutex> lock(input_cv_mutex_);
        if (input_pending_) {
            input_value_ = value;
            input_buf_.clear();
            input_pending_ = false;
            input_submitted_ = true;
            input_cv_.notify_one();
            return;
        }
        if (!busy_.load()) return;

        std::string trimmed = value;
        size_t first = trimmed.find_first_not_of(" \t");
        if (first != std::string::npos) trimmed = trimmed.substr(first);
        if (trimmed == "/stop" || trimmed == "/help") {
            command = trimmed;
            handler = busy_command_handler_;
        }
        input_buf_.clear();
    }
    if (handler) handler(command);
}

void TerminalUi::set_busy(bool busy) {
    busy_.store(busy);
    request_redraw();
}

void TerminalUi::set_busy_command_handler(std::function<void(const std::string&)> handler) {
    std::lock_guard<std::mutex> lock(input_cv_mutex_);
    busy_command_handler_ = std::move(handler);
}

void TerminalUi::interrupt_confirm() {
    std::lock_guard<std::mutex> lock(confirm_mutex_);
    if (show_confirm_.load()) {
        confirm_result_ = false;
        confirm_done_ = true;
        show_confirm_.store(false);
        confirm_cv_.notify_one();
    }
}

void TerminalUi::notify_work_idle() {
    std::lock_guard<std::mutex> lock(model_mutex_);
    state_ = WorkState::Idle;
}

std::string TerminalUi::read_line_input() {
    std::unique_lock<std::mutex> lock(input_cv_mutex_);
    input_pending_ = true;
    input_submitted_ = false;
    while (!input_cv_.wait_for(lock, std::chrono::seconds(2),
                               [this] { return input_submitted_; })) {
        time_t hb = ui_last_heartbeat_.load();
        if (hb > 0 && std::time(nullptr) - hb > 10) {
            ftxui::ScreenInteractive* s = screen_.load();
            if (s) s->ExitLoopClosure()();
            input_pending_ = false;
            return "exit";
        }
    }
    input_pending_ = false;
    return input_value_;
}

// ============ 会话数据 ============

void TerminalUi::set_session_info(const std::string& session_id, const std::string& name,
                                  const std::string& provider, const std::string& model) {
    std::lock_guard<std::mutex> lock(model_mutex_);
    session_id_ = session_id;
    session_name_ = name;
    provider_name_ = provider;
    model_name_ = model;
}

void TerminalUi::set_compact_point() {
    std::lock_guard<std::mutex> lock(model_mutex_);
    compact_point_tokens_ = total_input_ + total_output_;
}

void TerminalUi::set_compact_result(const std::string& text) {
    std::lock_guard<std::mutex> lock(model_mutex_);
    for (auto it = blocks_.rbegin(); it != blocks_.rend(); ++it) {
        if (it->kind == BlockKind::Status && it->extra.empty() &&
            it->raw.find("\u5df2\u538b\u7f29") != std::string::npos) {
            it->extra = text;
            it->expanded = false;
            break;
        }
    }
    request_redraw();
}

void TerminalUi::update_usage(int64_t input, int64_t output) {
    std::lock_guard<std::mutex> lock(model_mutex_);
    turn_input_ += input;
    turn_output_ += output;
    total_input_ += input;
    total_output_ += output;
}

void TerminalUi::set_work_state(WorkState state, const std::string& tool_name) {
    // 调用方需已持有 model_mutex_。切勿在此重复加锁：on_agent_event 的
    // Message 分支在持锁状态下调用本函数，非递归 mutex 重复加锁会自死锁。
    state_ = state;
    state_tool_ = tool_name;
}

static std::string first_tool_name(const Message& msg) {
    for (const auto& block : msg.content) {
        if (!std::holds_alternative<ToolRequest>(block)) continue;
        const auto& req = std::get<ToolRequest>(block);
        if (std::holds_alternative<CallToolRequestParams>(req.tool_call)) {
            const auto& params = std::get<CallToolRequestParams>(req.tool_call);
            return params.name.empty() ? "?" : params.name;
        }
    }
    return "";
}

namespace {
const int kMaxToolOutputLines = 30;
const int kMaxToolOutputLineLen = 240;

std::string tool_response_text(const CallToolResult& result) {
    std::string out;
    for (const auto& part : result.content) {
        if (part.is_object() && part.value("type", "") == "text" && part.contains("text")) {
            if (!out.empty()) out += "\n";
            const auto& text = part["text"];
            if (text.is_string()) out += text.get_ref<const std::string&>();
        }
    }
    std::istringstream iss(out);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(iss, line)) lines.push_back(line);
    int cap = std::min<int>(lines.size(), kMaxToolOutputLines);
    std::string final;
    for (int i = 0; i < cap; i++) {
        std::string l = lines[i];
        if (static_cast<int>(l.size()) > kMaxToolOutputLineLen) {
            l = l.substr(0, kMaxToolOutputLineLen) + "\u2026";
        }
        final += l + "\n";
    }
    if (lines.size() > static_cast<size_t>(cap)) {
        final += "\u2026 (" + std::to_string(lines.size() - cap) + " \u884c\u5df2\u9690\u85cf)";
    }
    return final;
}

std::string toolArgSummary(const CallToolRequestParams& params) {
    if (params.arguments.is_null() || params.arguments.empty()) return "";
    if (params.arguments.contains("command") && params.arguments["command"].is_string()) {
        return strip_ansi(params.arguments["command"].get<std::string>());
    }
    if (params.arguments.contains("path") && params.arguments["path"].is_string()) {
        return strip_ansi(params.arguments["path"].get<std::string>());
    }
    return "";
}
} // namespace

void TerminalUi::close_streams() {
    // 调用方需已持有 model_mutex_。
    if (open_thinking_ >= 0 && static_cast<size_t>(open_thinking_) < blocks_.size()) {
        blocks_[open_thinking_].stream_open = false;
        blocks_[open_thinking_].rows.clear();
        blocks_[open_thinking_].rows_w = 0;
        open_thinking_ = -1;
    }
    if (open_reply_ >= 0 && static_cast<size_t>(open_reply_) < blocks_.size()) {
        blocks_[open_reply_].stream_open = false;
        blocks_[open_reply_].rows.clear();
        blocks_[open_reply_].rows_w = 0;
        open_reply_ = -1;
    }
}

void TerminalUi::on_agent_event(const AgentEvent& event) {
    spdlog::debug("on_agent_event type={}", static_cast<int>(event.type));
    bool need_redraw = false;
    switch (event.type) {
        case AgentEventType::ReasoningDelta: {
            std::lock_guard<std::mutex> lock(model_mutex_);
            if (open_thinking_ < 0) {
                close_streams();
                if (blocks_.size() >= kMaxBlocks) blocks_.erase(blocks_.begin());
                MsgBlock b;
                b.kind = BlockKind::Thinking;
                b.stream_open = true;
                blocks_.push_back(std::move(b));
                open_thinking_ = static_cast<int>(blocks_.size()) - 1;
                turn_streamed_reasoning_ = true;
                set_work_state(WorkState::Reasoning);
                need_redraw = true;
                spdlog::debug("  created thinking block idx={}", open_thinking_);
            }
            if (static_cast<size_t>(open_thinking_) < blocks_.size()) {
                blocks_[open_thinking_].raw += event.reasoning_delta;
            }
            break;
        }
        case AgentEventType::TextDelta: {
            std::lock_guard<std::mutex> lock(model_mutex_);
            if (open_reply_ < 0) {
                close_streams();
                if (blocks_.size() >= kMaxBlocks) blocks_.erase(blocks_.begin());
                MsgBlock b;
                b.kind = BlockKind::Reply;
                b.stream_open = true;
                blocks_.push_back(std::move(b));
                open_reply_ = static_cast<int>(blocks_.size()) - 1;
                turn_streamed_text_ = true;
                set_work_state(WorkState::Generating);
                need_redraw = true;
                spdlog::debug("  created reply block idx={}", open_reply_);
            }
            if (static_cast<size_t>(open_reply_) < blocks_.size()) {
                blocks_[open_reply_].raw += event.text_delta;
            }
            break;
        }
        case AgentEventType::Message: {
            spdlog::debug("  Message event");
            std::lock_guard<std::mutex> lock(model_mutex_);
            close_streams();
            need_redraw = true;
            if (event.msg) {
                const auto& content = event.msg->content;
                for (const auto& block : content) {
                    if (std::holds_alternative<ToolRequest>(block)) {
                        const auto& req = std::get<ToolRequest>(block);
                        std::string name, args;
                        if (std::holds_alternative<CallToolRequestParams>(req.tool_call)) {
                            const auto& params = std::get<CallToolRequestParams>(req.tool_call);
                            name = params.name;
                            args = toolArgSummary(params);
                        } else if (std::holds_alternative<std::string>(req.tool_call)) {
                            name = "[error]";
                            args = strip_ansi(std::get<std::string>(req.tool_call));
                        }
                        if (blocks_.size() >= kMaxBlocks) blocks_.erase(blocks_.begin());
                        MsgBlock b;
                        b.kind = BlockKind::ToolCall;
                        b.tool_name = name;
                        b.tool_args = args;
                        blocks_.push_back(std::move(b));
                    } else if (std::holds_alternative<ToolResponse>(block)) {
                        const auto& resp = std::get<ToolResponse>(block);
                        if (std::holds_alternative<CallToolResult>(resp.tool_result)) {
                            const auto& result = std::get<CallToolResult>(resp.tool_result);
                            for (int i = static_cast<int>(blocks_.size()) - 1; i >= 0; i--) {
                                if (blocks_[i].kind == BlockKind::ToolCall) {
                                    blocks_[i].tool_has_output = true;
                                    blocks_[i].tool_output = tool_response_text(result);
                                    break;
                                }
                            }
                        }
                    } else if (std::holds_alternative<TextContent>(block)) {
                        if (!turn_streamed_text_) {
                            if (blocks_.size() >= kMaxBlocks) blocks_.erase(blocks_.begin());
                            MsgBlock b;
                            b.kind = BlockKind::Reply;
                            b.raw = std::get<TextContent>(block).text;
                            blocks_.push_back(std::move(b));
                            turn_streamed_text_ = true;
                        }
                    } else if (std::holds_alternative<ThinkingContent>(block)) {
                        if (!turn_streamed_reasoning_) {
                            if (blocks_.size() >= kMaxBlocks) blocks_.erase(blocks_.begin());
                            MsgBlock b;
                            b.kind = BlockKind::Thinking;
                            b.raw = std::get<ThinkingContent>(block).thinking;
                            blocks_.push_back(std::move(b));
                            turn_streamed_reasoning_ = true;
                        }
                    }
                }
            }
            if (event.msg && !first_tool_name(*event.msg).empty()) {
                set_work_state(WorkState::Tool, "");
            } else {
                set_work_state(turn_streamed_text_ ? WorkState::Done : WorkState::Idle);
            }
            break;
        }
        case AgentEventType::Usage:
            if (event.provider_usage) {
                const auto& usage = event.provider_usage->usage;
                update_usage(usage.input_tokens.value_or(0), usage.output_tokens.value_or(0));
            }
            break;
    }
    if (need_redraw) {
        spdlog::debug("on_agent_event done, redraw");
        request_redraw();
    } else {
        spdlog::debug("on_agent_event done, skip redraw (refresh timer)");
    }
}

void TerminalUi::append_status_text(const std::string& text) {
    std::lock_guard<std::mutex> lock(model_mutex_);
    close_streams();
    if (blocks_.size() >= kMaxBlocks) blocks_.erase(blocks_.begin());
    MsgBlock b;
    b.kind = BlockKind::Status;
    b.raw = text;
    blocks_.push_back(std::move(b));
    request_redraw();
}

void TerminalUi::append_user_message(const std::string& text) {
    std::lock_guard<std::mutex> lock(model_mutex_);
    close_streams();
    if (blocks_.size() >= kMaxBlocks) blocks_.erase(blocks_.begin());
    MsgBlock b;
    b.kind = BlockKind::User;
    b.raw = text;
    blocks_.push_back(std::move(b));
    request_redraw();
}

void TerminalUi::begin_assistant_turn() {
    std::lock_guard<std::mutex> lock(model_mutex_);
    close_streams();
    turn_streamed_reasoning_ = false;
    turn_streamed_text_ = false;
    request_redraw();
}

// ============ 刷新 ============

void TerminalUi::tick() {
    tick_count_.fetch_add(1);
    request_redraw();
}

void TerminalUi::start_refresh() {
    if (!refresh_stop_.exchange(false)) return;
    refresh_thread_ = std::thread([this] { refresh_loop(); });
}

void TerminalUi::refresh_loop() {
    spdlog::debug("refresh_loop started");
    int64_t stuck_ms = 0;
    uint64_t tick = 0;
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (refresh_stop_.load()) {
            spdlog::debug("refresh_loop stopped");
            break;
        }
        tick_count_.fetch_add(1);
        request_redraw();
        spdlog::debug("refresh tick {}", ++tick);

        // 看门狗：UI 线程停止更新 stage 超过 2s 说明它卡住或退出了。
        // 只在临界点打印一次，避免刷屏。
        int64_t age_ms = (g_render_stage_ns.load() != 0) ? stage_age_ns() / 1000000 : 0;
        bool stuck = g_render_stage_ns.load() != 0 && age_ms > 2000;
        if (stuck && stuck_ms < 2000) {
            stuck_ms = 2000;
            spdlog::warn("watchdog: UI thread STUCK at stage '{}' for {}ms (no render progress)", g_render_stage.load(), age_ms);
        } else if (!stuck && stuck_ms != 0) {
            stuck_ms = 0;
            spdlog::info("watchdog: UI thread recovered at stage '{}'", g_render_stage.load());
        }
    }
}

void TerminalUi::stop_refresh() {
    refresh_stop_.store(true);
    if (refresh_thread_.joinable()) {
        refresh_thread_.join();
    }
}

}} // namespace goose::cli