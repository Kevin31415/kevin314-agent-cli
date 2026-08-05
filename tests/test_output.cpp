#include <gtest/gtest.h>
#include <string>
#include "cli/session/output.h"

using goose::cli::MarkdownStream;
using goose::cli::render_markdown;

static const char* BOLD = "\033[1m";
static const char* ITALIC = "\033[3m";
static const char* UNDERLINE = "\033[4m";
static const char* RESET = "\033[0m";
static const char* BLUE = "\033[34m";
static const char* CYAN = "\033[36m";
static const char* GREEN = "\033[32m";
static const char* DIM = "\033[2m";

TEST(MarkdownRenderTest, Bold) {
    std::string out = render_markdown("这是 **加粗** 文本\n");
    EXPECT_NE(out.find(std::string(BOLD) + "加粗" + RESET), std::string::npos);
}

TEST(MarkdownRenderTest, Italic) {
    std::string out = render_markdown("这是 *斜体* 文本\n");
    EXPECT_NE(out.find(std::string(ITALIC) + "斜体" + RESET), std::string::npos);
}

TEST(MarkdownRenderTest, InlineCode) {
    std::string out = render_markdown("运行 `kacli run -t foo` 即可\n");
    EXPECT_NE(out.find(std::string(CYAN) + "kacli run -t foo" + RESET), std::string::npos);
}

TEST(MarkdownRenderTest, Link) {
    std::string out = render_markdown("参见 [文档](https://example.com)\n");
    EXPECT_NE(out.find(std::string(UNDERLINE) + BLUE + "文档"), std::string::npos);
}

TEST(MarkdownRenderTest, NestedBoldWithCode) {
    std::string out = render_markdown("**这是 `goose-c++` 项目的一个重构**\n");
    EXPECT_NE(out.find(std::string(BOLD) + "这是 " + CYAN + "goose-c++" + RESET + " 项目的一个重构" + RESET),
              std::string::npos);
}

TEST(MarkdownRenderTest, NestedBoldWithItalic) {
    std::string out = render_markdown("**含 *斜体* 的粗体**\n");
    EXPECT_NE(out.find(std::string(BOLD) + "含 " + ITALIC + "斜体" + RESET + " 的粗体" + RESET),
              std::string::npos);
}

TEST(MarkdownRenderTest, ItalicInsideLinkText) {
    std::string out = render_markdown("参见 [*文档*](https://example.com)\n");
    EXPECT_NE(out.find(std::string(UNDERLINE) + BLUE + ITALIC + "文档" + RESET + RESET),
              std::string::npos);
}

TEST(MarkdownRenderTest, UnclosedBoldNotDestroyingLine) {
    std::string out = render_markdown("**未闭合的加粗与后面文本\n");
    EXPECT_NE(out.find("**未闭合的加粗与后面文本"), std::string::npos);
}

TEST(MarkdownRenderTest, ListItemWithInlineMarkup) {
    std::string out = render_markdown("- **C++ 项目**，使用 **CMake** 作为构建系统\n");
    EXPECT_NE(out.find(std::string(GREEN) + "  • " + RESET + BOLD + "C++ 项目" + RESET),
              std::string::npos);
    EXPECT_NE(out.find(std::string(BOLD) + "CMake" + RESET), std::string::npos);
}

TEST(MarkdownRenderTest, NestedIndentedListWithCode) {
    std::string out = render_markdown("  - `src` 目录保存了 **编译产物**\n");
    EXPECT_NE(out.find(std::string(GREEN) + "  • " + RESET + CYAN + "src" + RESET),
              std::string::npos);
    EXPECT_NE(out.find(std::string(BOLD) + "编译产物" + RESET), std::string::npos);
}

TEST(MarkdownRenderTest, Utf8BulletList) {
    std::string out = render_markdown("• 使用 **CMake** 构建\n");
    EXPECT_NE(out.find(std::string(GREEN) + "  • " + RESET + "使用 " + BOLD + "CMake" + RESET),
              std::string::npos);
}

TEST(MarkdownRenderTest, QuoteWithInlineMarkup) {
    std::string out = render_markdown("> 建议使用 `cmake` 构建\n");
    EXPECT_NE(out.find(std::string(DIM) + "│ " + RESET + ITALIC + "建议使用 " + CYAN + "cmake" + RESET),
              std::string::npos);
}

TEST(MarkdownRenderTest, Heading) {
    std::string out = render_markdown("## 二级标题\n");
    EXPECT_NE(out.find(std::string(BOLD) + CYAN + "二级标题" + RESET), std::string::npos);

    out = render_markdown("### 三级标题\n");
    EXPECT_NE(out.find(std::string(BOLD) + "三级标题" + RESET), std::string::npos);
}

TEST(MarkdownRenderTest, CodeBlock) {
    std::string md = "```cpp\nint main() { return 0; }\n```\n";
    std::string out = render_markdown(md);
    EXPECT_NE(out.find("┌─ cpp"), std::string::npos);
    EXPECT_NE(out.find(std::string(DIM) + "│ " + RESET + "int main() { return 0; }"), std::string::npos);
    EXPECT_NE(out.find("└─"), std::string::npos);
}

TEST(MarkdownRenderTest, ListAndQuote) {
    std::string out = render_markdown("- 项目一\n> 引用内容\n");
    EXPECT_NE(out.find(std::string(GREEN) + "  • " + RESET + "项目一"), std::string::npos);
    EXPECT_NE(out.find(std::string(DIM) + "│ " + RESET + ITALIC + "引用内容" + RESET), std::string::npos);
}

TEST(MarkdownStreamTest, BoldSplitAcrossDeltas) {
    MarkdownStream stream;
    std::string part1 = stream.feed("这是 **加");
    EXPECT_EQ(part1, "");
    std::string part2 = stream.feed("粗** 文本\n");
    EXPECT_NE(part2.find(std::string(BOLD) + "加粗" + RESET), std::string::npos);
}

TEST(MarkdownStreamTest, CodeBlockAcrossDeltas) {
    MarkdownStream stream;
    std::string start = stream.feed("```cpp\n");
    EXPECT_NE(start.find("┌─ cpp"), std::string::npos);

    std::string body = stream.feed("int x = 1;\n```\n");
    EXPECT_NE(body.find(std::string(DIM) + "│ " + RESET + "int x = 1;"), std::string::npos);
    EXPECT_NE(body.find("└─"), std::string::npos);
}

TEST(MarkdownStreamTest, FlushClosesUnfinishedCodeBlock) {
    MarkdownStream stream;
    std::string body = stream.feed("```python\nprint(1)\n");
    EXPECT_NE(body.find(std::string(DIM) + "│ " + RESET + "print(1)"), std::string::npos);
    std::string flushed = stream.flush();
    EXPECT_NE(flushed.find("└─"), std::string::npos);
}

TEST(MarkdownStreamTest, TrailingLineWithoutNewline) {
    MarkdownStream stream;
    std::string no_output = stream.feed("末尾没有换行的行");
    EXPECT_EQ(no_output, "");
    std::string flushed = stream.flush();
    EXPECT_NE(flushed.find("末尾没有换行的行"), std::string::npos);
}

TEST(MarkdownStreamTest, HeadingAcrossDeltas) {
    MarkdownStream stream;
    stream.feed("# 大");
    std::string rest = stream.feed("标题\n正文内容\n");
    EXPECT_NE(rest.find(std::string(BOLD) + CYAN + "大标题" + RESET), std::string::npos);
}
