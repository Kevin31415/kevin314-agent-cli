#include <gtest/gtest.h>
#include "../src/cli/session/terminal_ui.h"

using namespace goose::cli;

TEST(TerminalUiUtils, AsciiWidth) {
    EXPECT_EQ(utf8_char_width('a'), 1);
    EXPECT_EQ(utf8_char_width(' '), 1);
    EXPECT_EQ(utf8_char_width(0x7F), 0);
}

TEST(TerminalUiUtils, WideCharWidth) {
    EXPECT_EQ(utf8_char_width(0x4E2D), 2);  // 中
    EXPECT_EQ(utf8_char_width(0x4F60), 2);  // 你
    EXPECT_EQ(utf8_char_width(0xAC00), 2);  // 가
    EXPECT_EQ(utf8_char_width(0x00E9), 1);  // é
}

TEST(TerminalUiUtils, DecodeUtf8) {
    size_t i = 0;
    std::string s = "a中";
    EXPECT_EQ(utf8_decode_next(s, i), 0x61);
    EXPECT_EQ(i, 1);
    EXPECT_EQ(utf8_decode_next(s, i), 0x4E2D);
    EXPECT_EQ(i, 4);
}

TEST(TerminalUiUtils, TruncateFits) {
    EXPECT_EQ(truncate_by_width("hello", 10), "hello");
    EXPECT_EQ(truncate_by_width("", 5), "");
    EXPECT_EQ(truncate_by_width("abc", 3), "abc");
}

TEST(TerminalUiUtils, TruncateAscii) {
    std::string r = truncate_by_width("abcdefghij", 5);
    EXPECT_EQ(r, "abcd\u2026");
    EXPECT_EQ(utf8_char_width(0x2026), 1);
}

TEST(TerminalUiUtils, TruncateWideKeepsUtf8) {
    std::string r = truncate_by_width("中文测试abc", 5);
    // 中(2) 文(2) 测(2) 超 → 中文… (2+2+1)
    EXPECT_EQ(r, "中文\u2026");
}

TEST(TerminalUiUtils, TruncateWideEvenBoundary) {
    std::string r = truncate_by_width("中文测试", 4);
    // 中(2) 文(2) 会占满 4 列，无余位放省略号 → 中…
    EXPECT_EQ(r, "中\u2026");
}

TEST(TerminalUiUtils, TruncateMixed) {
    std::string r = truncate_by_width("ab中文cd", 4);
    // a(1) b(1) 中(2) 文(2) 超 → ab…
    EXPECT_EQ(r, "ab\u2026");
}

TEST(TerminalUiUtils, WrapAscii) {
    EXPECT_EQ(wrap_by_width("abcdefghij", 5), "abcde\nfghij");
    EXPECT_EQ(wrap_by_width("short", 10), "short");
}

TEST(TerminalUiUtils, WrapWideCjk) {
    // 中a(3列) 放满 4 列？ 中(2)+a(1)=3，下一个 中(2) 放不下 → 换行
    EXPECT_EQ(wrap_by_width("中a中b", 4), "中a\n中b");
    // 中(2)+文(2)=4 满宽，下一行也是 4 → 不换行
    EXPECT_EQ(wrap_by_width("中文中文", 4), "中文\n中文");
}

TEST(TerminalUiUtils, WrapOverlongSingleChar) {
    // 中(2)+a(1)=3 占满第 1 行；bcd 和 ef 各 3 列。
    std::string w = wrap_by_width("中abcdef", 3);
    EXPECT_EQ(w, "中a\nbcd\nef");
}

TEST(TerminalUiUtils, WrapNoWidth) {
    EXPECT_EQ(wrap_by_width("abc", 0), "abc");
    EXPECT_EQ(wrap_by_width("abc", -1), "abc");
}

TEST(TerminalUiUtils, FormatTokenUnits) {
    EXPECT_EQ(format_token(0), "0");
    EXPECT_EQ(format_token(999), "999");
    EXPECT_EQ(format_token(1000), "1.0K");
    EXPECT_EQ(format_token(12345), "12.3K");
    EXPECT_EQ(format_token(999999), "1000.0K");
    EXPECT_EQ(format_token(1000000), "1.0M");
    EXPECT_EQ(format_token(1234567), "1.2M");
    EXPECT_EQ(format_token(-5), "0");
}

TEST(TerminalUiUtils, PadToWidth) {
    EXPECT_EQ(pad_or_truncate_to_width("abc", 5), "abc  ");
    EXPECT_EQ(pad_or_truncate_to_width("abc", 3), "abc");
    EXPECT_EQ(pad_or_truncate_to_width("", 2), "  ");
    // 顶栏状态段：不追加省略号（bug: truncate_by_width 会补 …）
    EXPECT_EQ(pad_or_truncate_to_width("\xe5\xbe\x85\xe8\xbe\x93\xe5\x85\xa5", 9), "\xe5\xbe\x85\xe8\xbe\x93\xe5\x85\xa5   ");
    EXPECT_EQ(pad_or_truncate_to_width("待输入……", 9), "待输入…… ");
}

TEST(TerminalUiUtils, PadNoEllipsisOverflow) {
    std::string r = pad_or_truncate_to_width("abcdefghij", 5);
    EXPECT_EQ(r, "abcde");
    EXPECT_EQ(r.find("\u2026"), std::string::npos);
    // 全角：中(2)+文(2)=4 可放，测(2) 超 → 保留 "中文"，无省略号
    std::string r2 = pad_or_truncate_to_width("中文测试", 4);
    EXPECT_EQ(r2, "中文");
}

namespace goose {
namespace cli {
struct MdRenderTestAccess {
    static std::vector<TerminalUi::StyledRow> render(const std::string& raw, int max_cols) {
        bool in_code = false;
        std::string lang;
        TerminalUi ui;
        return ui.render_md_rows(raw, in_code, lang, ftxui::Color::White, max_cols);
    }
    static std::string dump(const std::vector<TerminalUi::StyledRow>& rows) {
        std::string out;
        for (const auto& r : rows) {
            for (const auto& p : r) out += p.text;
            out += '\n';
        }
        return out;
    }
};
}  // namespace cli
}  // namespace goose

using Md = goose::cli::MdRenderTestAccess;

TEST(TerminalUiMarkdown, TableBasic) {
    std::string md =
        "| 名称 | 数量 | 价格 |\n"
        "|:----:|:----:|------:|\n"
        "| 苹果 | 3    | 6.5   |\n"
        "| 香蕉 | 5    | 2.0   |\n";
    std::string d = Md::dump(Md::render(md, 100));
    EXPECT_NE(d.find("\u256d"), std::string::npos);  // ┌
    EXPECT_NE(d.find("\u252c"), std::string::npos);  // ┬
    EXPECT_NE(d.find("\u251c"), std::string::npos);  // ├
    EXPECT_NE(d.find("\u256f"), std::string::npos);  // ┘
    EXPECT_NE(d.find("苹果"), std::string::npos);
    EXPECT_NE(d.find("香蕉"), std::string::npos);
    EXPECT_NE(d.find("6.5"), std::string::npos);
    EXPECT_NE(d.find("2.0"), std::string::npos);
}

TEST(TerminalUiMarkdown, TableAlignRight) {
    std::string md =
        "| 名称 | 值 |\n"
        "|:-----|----:|\n"
        "| A    | 1   |\n"
        "| BBB  | 100 |\n";
    std::string d = Md::dump(Md::render(md, 100));
    // 值列右对齐：短值 "1" 前补空格（列宽 3）
    EXPECT_NE(d.find("\u2502   1 \u2502"), std::string::npos);
    // 名称列左对齐：短值 "A" 后补空格（列宽 4，因表头"名称"占 4 列）
    EXPECT_NE(d.find("\u2502 A    \u2502"), std::string::npos);
}

TEST(TerminalUiMarkdown, TableNoPipePlainLine) {
    std::string md = "这只是一段普通文本没有管道符\n";
    std::string d = Md::dump(Md::render(md, 100));
    EXPECT_EQ(d.find("\u256d"), std::string::npos);
    EXPECT_NE(d.find("普通文本"), std::string::npos);
}

TEST(TerminalUiMarkdown, NestedListIndent) {
    std::string md =
        "- 第一层\n"
        "  - 第二层\n"
        "    - 第三层\n";
    std::string d = Md::dump(Md::render(md, 100));
    EXPECT_NE(d.find("\u2022 第一层"), std::string::npos);
    EXPECT_NE(d.find("  \u2022 第二层"), std::string::npos);
    EXPECT_NE(d.find("    \u2022 第三层"), std::string::npos);
}

TEST(TerminalUiMarkdown, TaskList) {
    std::string md =
        "- [x] 已完成\n"
        "- [ ] 待办\n";
    std::string d = Md::dump(Md::render(md, 100));
    EXPECT_NE(d.find("\u2611 已完成"), std::string::npos);  // ☑
    EXPECT_NE(d.find("\u2610 待办"), std::string::npos);    // ☐
}

TEST(TerminalUiMarkdown, OrderedList) {
    std::string md =
        "1. 第一\n"
        "2. 第二\n";
    std::string d = Md::dump(Md::render(md, 100));
    EXPECT_NE(d.find("1. 第一"), std::string::npos);
    EXPECT_NE(d.find("2. 第二"), std::string::npos);
}

TEST(TerminalUiMarkdown, NestedQuote) {
    std::string md =
        "> 一级引用\n"
        ">> 二级引用\n";
    std::string d = Md::dump(Md::render(md, 100));
    EXPECT_NE(d.find("\u2502 一级引用"), std::string::npos);
    EXPECT_NE(d.find(" \u2502 二级引用"), std::string::npos);
}

TEST(TerminalUiMarkdown, UnderscoreItalicBold) {
    std::string md = "_斜体_ 和 __粗体__ 以及普通\n";
    std::string d = Md::dump(Md::render(md, 100));
    EXPECT_NE(d.find("斜体"), std::string::npos);
    EXPECT_EQ(d.find("_斜体_"), std::string::npos);
    EXPECT_NE(d.find("粗体"), std::string::npos);
    EXPECT_EQ(d.find("__粗体__"), std::string::npos);
}

TEST(TerminalUiMarkdown, UnderscoreInWordKept) {
    // 单词内下划线不当强调（变量名）
    std::string md = "foo_bar_baz 和 _真正斜体_\n";
    std::string d = Md::dump(Md::render(md, 100));
    EXPECT_NE(d.find("foo_bar_baz"), std::string::npos);
    EXPECT_EQ(d.find("_真正斜体_"), std::string::npos);
}

TEST(TerminalUiMarkdown, TableThenListNoInterference) {
    std::string md =
        "| A | B |\n"
        "|---|--:|\n"
        "| 1 | 2 |\n"
        "- 表格后的列表\n";
    std::string d = Md::dump(Md::render(md, 100));
    EXPECT_NE(d.find("\u256d"), std::string::npos);
    EXPECT_NE(d.find("\u2022 表格后的列表"), std::string::npos);
}
