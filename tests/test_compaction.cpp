#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <vector>

#include "core/compaction.h"

using namespace goose;

namespace {

constexpr const char* kFullResponse = R"(<analysis>
The user asked to fix a bug in parser.rs. I traced it to an off-by-one
in {brace handling} and patched it.
</analysis>

```json
{
  "user_intent": ["Fix the parser bug", "Add a regression test"],
  "technical_concepts": ["off-by-one", "tokenizer"],
  "files": [
    {"path": "src/parser.rs", "summary": "Fixed off-by-one in scan loop", "key_code": "fn scan(&mut self) { .. }"}
  ],
  "errors_and_fixes": ["Panic on empty input, fixed with early return"],
  "problem_solving": ["Root-caused via failing unit test"],
  "user_messages": ["fix the parser bug", "add a test"],
  "pending_tasks": ["Add a regression test"],
  "current_work": "Writing the regression test in tests/parser.rs",
  "next_step": "Finish the regression test"
}
```)";

class MockProvider : public Provider {
public:
    std::string name = "mock";
    std::string response_text;
    int max_tool_responses = -1;
    std::function<Result<MessageStream>(const std::vector<Message>&)> stream_override;

    const std::string& get_name() const override { return name; }

    Result<MessageStream> stream(
        const ModelConfig& /*model_config*/,
        const std::string& /*system_prompt*/,
        const std::vector<Message>& messages,
        const std::vector<Tool>& /*tools*/) override {

        if (stream_override) return stream_override(messages);

        if (max_tool_responses >= 0) {
            int tool_response_count = 0;
            for (const auto& msg : messages) {
                for (const auto& block : msg.content) {
                    if (std::holds_alternative<ToolResponse>(block)) tool_response_count++;
                }
            }
            if (tool_response_count > max_tool_responses) {
                return Result<MessageStream>::err(make_error(
                    ErrorCode::ContextLengthError, "Too many tool responses"));
            }
        }

        bool first_chunk = true;
        MessageStream stream = [this, first_chunk]() mutable -> Result<StreamChunk> {
            if (first_chunk) {
                first_chunk = false;
                Message m = Message::assistant().with_text(response_text);
                return Result<StreamChunk>::ok(StreamChunk{m, std::nullopt});
            }
            return Result<StreamChunk>::err(make_error(ErrorCode::StreamEnd, "end"));
        };
        return Result<MessageStream>::ok(std::move(stream));
    }

private:
};

Conversation make_tool_conversation(size_t pairs) {
    std::vector<Message> messages{Message::user().with_text("start")};
    for (size_t i = 0; i < pairs; ++i) {
        ToolRequest req;
        req.id = "tool_" + std::to_string(i);
        CallToolRequestParams params;
        params.name = "read_file";
        params.arguments = nlohmann::json{{"path", "/tmp/x"}};
        req.tool_call = params;
        Message assistant = Message::assistant();
        assistant.content.push_back(std::move(req));
        messages.push_back(std::move(assistant));

        ToolResponse res;
        res.id = "tool_" + std::to_string(i);
        CallToolResult result;
        result.content.push_back(nlohmann::json{{"type", "text"}, {"text", "content"}});
        res.tool_result = std::move(result);
        Message user = Message::user();
        user.content.push_back(std::move(res));
        messages.push_back(std::move(user));
    }
    return Conversation(std::move(messages));
}

} // namespace

// ============ StructuredSummary parsing ============

TEST(CompactionParse, parses_fenced_json_after_analysis) {
    auto summary = parse_structured_summary(kFullResponse);
    ASSERT_TRUE(summary.has_value());
    EXPECT_EQ(summary->user_intent, (std::vector<std::string>{"Fix the parser bug", "Add a regression test"}));
    ASSERT_EQ(summary->files.size(), 1);
    EXPECT_EQ(summary->files[0].path, "src/parser.rs");
    EXPECT_EQ(summary->current_work, std::optional<std::string>("Writing the regression test in tests/parser.rs"));
}

TEST(CompactionParse, unusable_responses_fall_back_to_raw_text) {
    for (const char* text : {
             // freeform prose, no JSON document
             "Here is a summary of the conversation. The user asked about compaction.",
             // no visible content
             "{}",
             R"({"notes": "unknown fields alone are not a summary"})",
             R"({"current_work": ""})",
             R"({"files": [{}], "user_intent": [" "]})",
             // output cut off mid-JSON: never repaired
             "```json\n{\"user_intent\": [\"Fix the bug\"], \"pending_tasks\": [\"Write tests\", \"Update docs\"",
             // JSON quoted inside prose is not anchored at a marker
             "The session focused on the parser migration. The tracker entry {\"current_work\": \"migrate parser\"} is unchanged, and tests still need porting.",
             "<analysis>reviewing</analysis>\nA prose recap: the config was set to {\"user_intent\": [\"quoted example\"]} per the docs, then the run passed.",
             // a fenced example inside the scratchpad is not the summary
             "<analysis>\nThe target shape is:\n```json\n{\"user_intent\": [\"example only\"]}\n```\nNow let me review the conversation.\n</analysis>\nSorry, I ran out of room and could not produce the summary document.",
             // a quoted terminator inside the scratchpad must not expose its fenced example
             "<analysis>\nThe prompt ends with </analysis> and shows the shape:\n```json\n{\"user_intent\": [\"example only\"]}\n```\nNow let me review the conversation.\n</analysis>\nSorry, I ran out of room and could not produce the summary document.",
         }) {
        EXPECT_FALSE(parse_structured_summary(text).has_value()) << "should fall back to raw text for: " << text;
    }
}

TEST(CompactionParse, embedded_fences_in_string_values_do_not_break_extraction) {
    auto summary = parse_structured_summary(
        "```json\n{\"user_intent\": [\"Document the build\"], \"files\": [{\"path\": \"README.md\", \"summary\": \"Added build docs\", \"key_code\": \"```bash\\ncargo build\\n```\"}], \"pending_tasks\": [\"Publish the docs\"]}\n```");
    ASSERT_TRUE(summary.has_value());
    ASSERT_EQ(summary->files.size(), 1);
    EXPECT_EQ(summary->files[0].key_code, std::optional<std::string>("```bash\ncargo build\n```"));
    EXPECT_EQ(summary->pending_tasks, (std::vector<std::string>{"Publish the docs"}));
}

TEST(CompactionParse, quoted_terminator_inside_summary_json_does_not_hide_it) {
    auto summary = parse_structured_summary(
        "<analysis>\nThe session edited the compaction prompt itself.\n</analysis>\n```json\n{\"user_intent\": [\"Rework the scratchpad prompt\"], \"files\": [{\"path\": \"prompts/compact.md\", \"summary\": \"Tightened the <analysis>...</analysis> instructions\"}]}\n```");
    ASSERT_TRUE(summary.has_value());
    EXPECT_EQ(summary->user_intent, (std::vector<std::string>{"Rework the scratchpad prompt"}));
    EXPECT_EQ(summary->files[0].summary, "Tightened the <analysis>...</analysis> instructions");
}

TEST(CompactionParse, retries_next_candidate_when_fenced_extraction_fails) {
    auto summary = parse_structured_summary(
        "<analysis>the model was told to emit ```json with {braces</analysis>\n{\"user_intent\": [\"Real goal\"]}");
    ASSERT_TRUE(summary.has_value());
    EXPECT_EQ(summary->user_intent, (std::vector<std::string>{"Real goal"}));
}

TEST(CompactionParse, lenient_shapes_are_stringified_not_rejected) {
    auto summary = parse_structured_summary(R"({
        "user_intent": "fix the flaky test",
        "errors_and_fixes": [
            {"error": "cursor drifted after replay batch 34", "fix": "bounded mpsc channel"},
            "plain string entry",
            null
        ],
        "pending_tasks": [42],
        "current_work": {"task": "regression test", "status": "in progress"}
    })");
    ASSERT_TRUE(summary.has_value());
    EXPECT_EQ(summary->user_intent, (std::vector<std::string>{"fix the flaky test"}));
    EXPECT_EQ(summary->errors_and_fixes,
              (std::vector<std::string>{
                  "error: cursor drifted after replay batch 34; fix: bounded mpsc channel",
                  "plain string entry"}));
    EXPECT_EQ(summary->pending_tasks, (std::vector<std::string>{"42"}));
    EXPECT_EQ(summary->current_work,
              std::optional<std::string>("status: in progress; task: regression test"));
}

TEST(CompactionParse, file_entries_parse_leniently) {
    auto summary = parse_structured_summary(R"({"files": [
        "src/parser.rs",
        {"path": "tests/parser.rs", "summary": "Added regression test"},
        {"path": "src/scan.rs", "summary": 42, "key_code": ["fn a() {}", "fn b() {}"]},
        ""
    ]})");
    ASSERT_TRUE(summary.has_value());
    ASSERT_EQ(summary->files.size(), 3);
    EXPECT_EQ(summary->files[0].path, "src/parser.rs");
    EXPECT_EQ(summary->files[0].summary, "");
    EXPECT_EQ(summary->files[1].summary, "Added regression test");
    EXPECT_EQ(summary->files[2].summary, "42");
    EXPECT_EQ(summary->files[2].key_code, std::optional<std::string>("fn a() {}; fn b() {}"));
}

TEST(CompactionParse, drops_blank_entries_but_keeps_content) {
    auto summary = parse_structured_summary(
        R"({"user_intent": ["", "Fix the bug"], "files": [{"path": "a.rs", "summary": "Patched", "key_code": "  "}], "next_step": " "})");
    ASSERT_TRUE(summary.has_value());
    EXPECT_EQ(summary->user_intent, (std::vector<std::string>{"Fix the bug"}));
    EXPECT_EQ(summary->files[0].key_code, std::nullopt);
    EXPECT_EQ(summary->next_step, std::nullopt);
}

// ============ StructuredSummary rendering ============

TEST(CompactionRender, renders_markdown_sections) {
    auto summary = parse_structured_summary(kFullResponse);
    ASSERT_TRUE(summary.has_value());
    auto rendered = render_structured_summary(*summary);
    ASSERT_TRUE(rendered);
    EXPECT_NE(rendered->find("## 用户意图"), std::string::npos);
    EXPECT_NE(rendered->find("- Fix the parser bug"), std::string::npos);
    EXPECT_NE(rendered->find("### src/parser.rs"), std::string::npos);
    EXPECT_NE(rendered->find("fn scan(&mut self) { .. }"), std::string::npos);
    EXPECT_NE(rendered->find("## 下一步"), std::string::npos);
}

TEST(CompactionRender, fences_exceed_backtick_runs_in_key_code) {
    StructuredSummary summary;
    summary.files.push_back(FileActivity{
        "docs/build.md",
        "Documented the build",
        std::optional<std::string>("```bash\ncargo build\n```\n````\nnested fence docs\n````")});
    summary.errors_and_fixes.push_back("None");
    auto rendered = render_structured_summary(summary);
    ASSERT_TRUE(rendered);
    size_t closing = rendered->rfind("\n`````\n");
    EXPECT_NE(closing, std::string::npos);
    EXPECT_GT(rendered->find("## Errors + Fixes"), closing);
}

// ============ trigger check ============

TEST(CompactionTrigger, threshold_boundaries) {
    Conversation conv({Message::user().with_text(std::string(10000, 'a'))});
    // 10000 bytes -> ~3334 tokens.
    EXPECT_FALSE(check_if_compaction_needed(conv, 0.8, 128000));
    EXPECT_TRUE(check_if_compaction_needed(conv, 0.8, 4000));
    // Thresholds outside (0, 1) disable auto-compaction.
    EXPECT_FALSE(check_if_compaction_needed(conv, 0.0, 1));
    EXPECT_FALSE(check_if_compaction_needed(conv, 1.0, 1));
    EXPECT_FALSE(check_if_compaction_needed(conv, -1.0, 1));
    EXPECT_FALSE(check_if_compaction_needed(conv, 2.0, 1));
}

// ============ compact_messages ============

TEST(CompactionRun, structured_summary_is_rendered) {
    constexpr const char* structured_response = R"(<analysis>User asked to fix a bug; I patched parser.rs.</analysis>
```json
{
  "user_intent": ["Fix the parser bug"],
  "files": [{"path": "src/parser.rs", "summary": "Fixed off-by-one"}],
  "pending_tasks": ["Add a regression test"],
  "current_work": "Writing the regression test"
}
```)";
    MockProvider provider;
    provider.response_text = structured_response;
    ModelConfig model_config;
    model_config.model_name = "test";

    Conversation conversation({Message::user().with_text("fix the parser bug"),
                               Message::assistant().with_text("Looking into it")});
    auto result = compact_messages(conversation, provider, model_config, true);
    ASSERT_TRUE(result) << result.error().message;

    const auto& msgs = result->conversation.messages();
    ASSERT_EQ(msgs.size(), 2);
    const std::string& summary_text = msgs[0].as_concat_text();
    EXPECT_NE(summary_text.find("# 对话总结"), std::string::npos);
    EXPECT_NE(summary_text.find("## 用户意图"), std::string::npos);
    EXPECT_NE(summary_text.find("- Fix the parser bug"), std::string::npos);
    EXPECT_NE(summary_text.find("### src/parser.rs"), std::string::npos);
    EXPECT_EQ(summary_text.find("```json"), std::string::npos);
    EXPECT_EQ(summary_text.find("<analysis>"), std::string::npos);
    EXPECT_EQ(msgs[1].role, Role::Assistant);
    EXPECT_NE(msgs[1].as_concat_text().find("Your context was compacted"), std::string::npos);
}

TEST(CompactionRun, preserves_most_recent_text_user_message) {
    MockProvider provider;
    provider.response_text = "<mock summary>";
    ModelConfig model_config;
    model_config.model_name = "test";

    Conversation conversation({Message::user().with_text("start"),
                               Message::assistant().with_text("ok"),
                               Message::user().with_text("continue")});
    auto result = compact_messages(conversation, provider, model_config, false);
    ASSERT_TRUE(result);

    const auto& msgs = result->conversation.messages();
    ASSERT_EQ(msgs.size(), 3);
    EXPECT_EQ(msgs[0].role, Role::User);
    EXPECT_EQ(msgs[1].role, Role::Assistant);
    EXPECT_EQ(msgs[2].role, Role::User);
    EXPECT_EQ(msgs[2].as_concat_text(), "continue");
}

TEST(CompactionRun, manual_compact_does_not_preserve_user_message) {
    MockProvider provider;
    provider.response_text = "<mock summary>";
    ModelConfig model_config;
    model_config.model_name = "test";

    Conversation conversation({Message::user().with_text("start"),
                               Message::assistant().with_text("ok"),
                               Message::user().with_text("continue")});
    auto result = compact_messages(conversation, provider, model_config, true);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->conversation.messages().size(), 2);
}

TEST(CompactionRun, progressive_removal_on_context_exceeded) {
    MockProvider provider;
    provider.response_text = "<mock summary>";
    // Fail whenever more than 2 tool responses are present: forces the 10%
    // attempt to remove enough from the middle.
    provider.max_tool_responses = 2;
    ModelConfig model_config;
    model_config.model_name = "test";

    auto result = compact_messages(make_tool_conversation(10), provider, model_config, true);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_FALSE(result->conversation.messages().empty());
}

TEST(CompactionRun, fails_when_removing_all_tool_responses_is_not_enough) {
    MockProvider provider;
    provider.response_text = "<mock summary>";
    provider.max_tool_responses = -1;
    // Always fail with a context error regardless of filtering.
    provider.stream_override = [](const std::vector<Message>&) -> Result<MessageStream> {
        return Result<MessageStream>::err(
            make_error(ErrorCode::ContextLengthError, "still too long"));
    };
    ModelConfig model_config;
    model_config.model_name = "test";

    auto result = compact_messages(make_tool_conversation(3), provider, model_config, true);
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message.find("all tool responses"), std::string::npos);
}

TEST(CompactionRun, single_tool_response_is_removed_when_needed) {
    MockProvider provider;
    provider.response_text = "<mock summary>";
    // Only succeed once the single tool response is removed.
    provider.max_tool_responses = 0;
    ModelConfig model_config;
    model_config.model_name = "test";

    auto result = compact_messages(make_tool_conversation(1), provider, model_config, true);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_FALSE(result->conversation.messages().empty());
}

TEST(CompactionRun, raw_text_fallback_when_no_json_document) {
    MockProvider provider;
    provider.response_text = "Here is a prose summary of the conversation.";
    ModelConfig model_config;
    model_config.model_name = "test";

    Conversation conversation({Message::user().with_text("hello"),
                               Message::assistant().with_text("hi")});
    auto result = compact_messages(conversation, provider, model_config, true);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->conversation.messages()[0].as_concat_text(),
              "Here is a prose summary of the conversation.");
}

// ============ formatting ============

TEST(CompactionFormat, formats_message_blocks) {
    Message assistant = Message::assistant().with_text("before");
    ToolRequest req;
    req.id = "t1";
    CallToolRequestParams params;
    params.name = "read_file";
    params.arguments = nlohmann::json{{"path", "a.txt"}};
    req.tool_call = params;
    assistant.content.push_back(std::move(req));
    assistant.content.push_back(ImageContent{"image/png", ""});

    std::string out = format_message_for_compacting(assistant);
    EXPECT_NE(out.find("before"), std::string::npos);
    EXPECT_NE(out.find("tool_request(read_file): {\"path\":\"a.txt\"}"), std::string::npos);
    EXPECT_NE(out.find("[image: image/png]"), std::string::npos);

    Message empty = Message::user();
    EXPECT_EQ(format_message_for_compacting(empty), "[user]: <empty message>");
}

TEST(CompactionFormat, formats_tool_response) {
    Message user = Message::user();
    ToolResponse res;
    res.id = "t1";
    CallToolResult result;
    result.content.push_back(nlohmann::json{{"type", "text"}, {"text", "hello, world"}});
    res.tool_result = std::move(result);
    user.content.push_back(std::move(res));

    EXPECT_EQ(format_message_for_compacting(user), "[user]: tool_response: hello, world");
}
