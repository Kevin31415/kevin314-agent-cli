#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "core/moim.h"
#include "core/types.h"

using namespace goose;

namespace {

bool is_moim(const MessageContentBlock& block) {
    const auto* text = std::get_if<TextContent>(&block);
    return text && text->text.rfind("<turn-context>\n", 0) == 0;
}

Message text_user(const std::string& text) {
    return Message::user().with_text(text);
}

Message tool_response_user() {
    Message msg = Message::user();
    ToolResponse res;
    res.id = "t1";
    CallToolResult result;
    result.content.push_back(nlohmann::json{{"type", "text"}, {"text", "ok"}});
    res.tool_result = std::move(result);
    msg.content.push_back(std::move(res));
    return msg;
}

} // namespace

TEST(MoimSystemPromptBlock, explains_turn_context) {
    std::string block = system_prompt_block();
    EXPECT_NE(block.find("# Turn Context"), std::string::npos);
    EXPECT_NE(block.find("<turn-context>"), std::string::npos);
    EXPECT_NE(block.find("kacli"), std::string::npos);
}

TEST(MoimCompose, minimal_block_has_time_and_directory) {
    std::string block = compose_moim("/tmp/proj", 0, 0, 0.8, 0, 0);
    EXPECT_EQ(block.rfind("<turn-context>\n", 0), 0);
    EXPECT_NE(block.find("<current-time>"), std::string::npos);
    EXPECT_NE(block.find("<working-directory>/tmp/proj</working-directory>"), std::string::npos);
    EXPECT_EQ(block.find("<compaction>"), std::string::npos);
    EXPECT_EQ(block.find("<turn-budget>"), std::string::npos);
    EXPECT_EQ(block.rfind("</turn-context>"), block.size() - std::string("</turn-context>").size());
}

TEST(MoimCompose, current_time_has_utc_offset) {
    std::string block = compose_moim("/tmp/proj", 0, 0, 0.8, 0, 0);
    auto time_line_pos = block.find("<current-time>");
    ASSERT_NE(time_line_pos, std::string::npos);
    auto value_start = time_line_pos + std::string("<current-time>").size();
    auto value_end = block.find("</current-time>", value_start);
    ASSERT_NE(value_end, std::string::npos);
    std::string value = block.substr(value_start, value_end - value_start);
    // %Y-%m-%d %H:%M:00 %z -> "2026-08-01 12:34:00 +0800"
    EXPECT_EQ(value.size(), 25);
    EXPECT_EQ(value[13], ':');
    EXPECT_EQ(value[16], ':');
    EXPECT_EQ(value.substr(17, 2), "00");
    EXPECT_EQ(value[19], ' ');
    EXPECT_EQ(value[20], '+');
}

TEST(MoimCompose, compaction_line_conditions) {
    // Below half of the compaction budget: hidden.
    auto hidden = compose_moim("/d", 40'000, 200'000, 0.8, 0, 0);
    EXPECT_EQ(hidden.find("<compaction>"), std::string::npos);
    // Exactly half: shown. Remaining = (160k - 80k) / 1000 = 80.
    auto shown = compose_moim("/d", 80'000, 200'000, 0.8, 0, 0);
    EXPECT_NE(shown.find("<compaction>~80k tokens remaining</compaction>"), std::string::npos);
    // Above half: remaining = (160k - 100k) / 1000 = 60.
    auto more = compose_moim("/d", 100'000, 200'000, 0.8, 0, 0);
    EXPECT_NE(more.find("<compaction>~60k tokens remaining</compaction>"), std::string::npos);
    // Disabled threshold: hidden.
    auto disabled = compose_moim("/d", 100'000, 200'000, 0.0, 0, 0);
    EXPECT_EQ(disabled.find("<compaction>"), std::string::npos);
    auto disabled2 = compose_moim("/d", 100'000, 200'000, 1.0, 0, 0);
    EXPECT_EQ(disabled2.find("<compaction>"), std::string::npos);
}

TEST(MoimCompose, turn_budget_line_conditions) {
    // Below half of the budget: hidden.
    auto hidden = compose_moim("/d", 0, 0, 0.8, 10, 40);
    EXPECT_EQ(hidden.find("<turn-budget>"), std::string::npos);
    // Half and above: shown.
    auto shown = compose_moim("/d", 0, 0, 0.8, 20, 40);
    EXPECT_NE(shown.find("<turn-budget>20/40 used</turn-budget>"), std::string::npos);
    // Unlimited turns: hidden.
    auto unlimited = compose_moim("/d", 0, 0, 0.8, 20, 0);
    EXPECT_EQ(unlimited.find("<turn-budget>"), std::string::npos);
}

TEST(MoimCompose, escapes_xml_special_characters) {
    std::string block = compose_moim("/d&<x>", 0, 0, 0.8, 0, 0);
    EXPECT_NE(block.find("<working-directory>/d&amp;&lt;x&gt;</working-directory>"),
              std::string::npos);
}

TEST(MoimInject, prepended_to_most_recent_text_user_message) {
    Conversation conv({text_user("Hello"),
                       Message::assistant().with_text("Hi"),
                       text_user("Bye")});
    auto result = inject_moim(conv, "/proj", 128'000, 0.8, 0, 100, 100);
    const auto& msgs = result.messages();
    ASSERT_EQ(msgs.size(), 3);
    EXPECT_EQ(msgs[0].as_concat_text(), "Hello");
    EXPECT_EQ(msgs[1].as_concat_text(), "Hi");
    EXPECT_TRUE(is_moim(msgs[2].content[0]));
    EXPECT_EQ(msgs[2].content.size(), 2);
    EXPECT_EQ(std::get<TextContent>(msgs[2].content[1]).text, "Bye");
}

TEST(MoimInject, single_user_message) {
    Conversation conv({text_user("Hello")});
    auto result = inject_moim(conv, "/proj", 128'000, 0.8, 0, 100, 100);
    ASSERT_EQ(result.messages().size(), 1);
    EXPECT_TRUE(is_moim(result.messages()[0].content[0]));
    EXPECT_EQ(result.messages()[0].content.size(), 2);
}

TEST(MoimInject, skips_tool_response_messages) {
    Conversation conv({tool_response_user(),
                       Message::assistant().with_text("done"),
                       text_user("continue")});
    auto result = inject_moim(conv, "/proj", 128'000, 0.8, 0, 100, 100);
    const auto& msgs = result.messages();
    ASSERT_EQ(msgs.size(), 3);
    // Tool response message untouched.
    EXPECT_EQ(msgs[0].content.size(), 1);
    EXPECT_TRUE(std::holds_alternative<ToolResponse>(msgs[0].content[0]));
    EXPECT_TRUE(is_moim(msgs[2].content[0]));
}

TEST(MoimInject, no_text_user_message_returns_unchanged) {
    Conversation conv({Message::assistant().with_text("Hi")});
    auto result = inject_moim(conv, "/proj", 128'000, 0.8, 0, 100, 100);
    EXPECT_EQ(result.messages().size(), 1);
    EXPECT_EQ(result.messages()[0].as_concat_text(), "Hi");
}

TEST(MoimInject, skips_when_context_too_small) {
    Conversation conv({text_user("Hello")});
    auto result = inject_moim(conv, "/proj", 31'999, 0.8, 0, 100, 100);
    EXPECT_EQ(result.messages()[0].as_concat_text(), "Hello");
    EXPECT_EQ(result.messages()[0].content.size(), 1);
}

TEST(MoimInject, empty_conversation_returns_unchanged) {
    Conversation conv;
    auto result = inject_moim(conv, "/proj", 128'000, 0.8, 0, 100, 100);
    EXPECT_TRUE(result.is_empty());
}
