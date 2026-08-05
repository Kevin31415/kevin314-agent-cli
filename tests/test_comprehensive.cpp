#include <gtest/gtest.h>
#include "../src/core/types.h"
#include "../src/core/tool.h"
#include "../src/core/agent_event.h"
#include "../src/provider/model_config.h"
#include "../src/provider/provider_usage.h"
#include "../src/config/config.h"
#include "../src/config/secrets.h"
#include "../src/config/paths.h"
#include "../src/session/session_manager.h"
#include "../src/extension/extension_config.h"
#include "../src/extension/extension_manager.h"
#include "../src/cli/session/input.h"
#include <nlohmann/json.hpp>

using namespace goose;
using namespace goose::cli;

// ========== AgentEvent tests ==========

TEST(AgentEventTest, MakeMessage) {
    Message msg = Message::assistant().with_text("hello");
    auto event = AgentEvent::make_message(msg);
    EXPECT_EQ(event.type, AgentEventType::Message);
    EXPECT_TRUE(event.msg.has_value());
    EXPECT_EQ(event.msg->as_concat_text(), "hello");
}

TEST(AgentEventTest, MakeTextDelta) {
    auto event = AgentEvent::make_text_delta("chunk1");
    EXPECT_EQ(event.type, AgentEventType::TextDelta);
    EXPECT_EQ(event.text_delta, "chunk1");
}

TEST(AgentEventTest, MakeUsage) {
    ProviderUsage pu;
    pu.provider = "openai";
    pu.usage = Usage{10, 20, std::nullopt, std::nullopt};
    auto event = AgentEvent::make_usage(pu);
    EXPECT_EQ(event.type, AgentEventType::Usage);
    EXPECT_TRUE(event.provider_usage.has_value());
    EXPECT_EQ(event.provider_usage->usage.input_tokens, 10);
}

// ========== Input parsing tests ==========

TEST(InputTest, EmptyInput) {
    auto result = parse_input("");
    EXPECT_EQ(result.type, InputResult::Retry);
}

TEST(InputTest, WhitespaceInput) {
    auto result = parse_input("   ");
    EXPECT_EQ(result.type, InputResult::Retry);
}

TEST(InputTest, ExitCommand) {
    auto result = parse_input("exit");
    EXPECT_EQ(result.type, InputResult::Exit);
}

TEST(InputTest, QuitCommand) {
    auto result = parse_input("quit");
    EXPECT_EQ(result.type, InputResult::Exit);
}

TEST(InputTest, SlashExit) {
    auto result = parse_input("/exit");
    EXPECT_EQ(result.type, InputResult::Exit);
}

TEST(InputTest, HelpCommand) {
    auto result = parse_input("/help");
    EXPECT_EQ(result.type, InputResult::SlashCommand);
    EXPECT_EQ(result.command, "/help");
}

TEST(InputTest, MessageInput) {
    auto result = parse_input("hello world");
    EXPECT_EQ(result.type, InputResult::Message);
    EXPECT_EQ(result.text, "hello world");
}

TEST(InputTest, SlashCommandWithArgs) {
    auto result = parse_input("/model gpt-4o");
    EXPECT_EQ(result.type, InputResult::SlashCommand);
    EXPECT_EQ(result.command, "/model");
    ASSERT_EQ(result.args.size(), 1u);
    EXPECT_EQ(result.args[0], "gpt-4o");
}

// ========== ModelConfig tests ==========

TEST(ModelConfigTest, DefaultConfig) {
    ModelConfig mc;
    mc.model_name = "gpt-4o";
    EXPECT_EQ(mc.model_name, "gpt-4o");
}

// ========== Usage tests ==========

TEST(UsageTest, Zero) {
    auto u = Usage::zero();
    EXPECT_EQ(u.input_tokens, 0);
    EXPECT_EQ(u.output_tokens, 0);
    EXPECT_FALSE(u.cache_read_input_tokens.has_value());
}

// ========== ExtensionConfig tests ==========

TEST(ExtensionConfigTest, StdioFactory) {
    auto ec = ExtensionConfig::stdio("ext1", "cmd1", {"arg1", "arg2"});
    EXPECT_EQ(ec.type, ExtensionConfig::Type::Stdio);
    EXPECT_EQ(ec.name, "ext1");
    EXPECT_EQ(ec.cmd, "cmd1");
    EXPECT_EQ(ec.args.size(), 2u);
}

TEST(ExtensionConfigTest, BuiltinFactory) {
    auto ec = ExtensionConfig::builtin("developer");
    EXPECT_EQ(ec.type, ExtensionConfig::Type::Builtin);
    EXPECT_EQ(ec.name, "developer");
}

// ========== Paths tests ==========

TEST(PathsTest, ConfigDir) {
    auto dir = paths::config_dir();
    EXPECT_FALSE(dir.empty());
}

TEST(PathsTest, DataDir) {
    auto dir = paths::data_dir();
    EXPECT_FALSE(dir.empty());
}

TEST(PathsTest, SessionDir) {
    auto dir = paths::session_dir();
    EXPECT_FALSE(dir.empty());
}

// ========== Conversation tests ==========

TEST(ConversationTest, PushAndLen) {
    Conversation conv;
    conv.push(Message::user().with_text("hello"));
    conv.push(Message::assistant().with_text("world"));
    EXPECT_EQ(conv.len(), 2u);
}

TEST(ConversationTest, IsEmpty) {
    Conversation conv;
    EXPECT_TRUE(conv.is_empty());
    conv.push(Message::user().with_text("hi"));
    EXPECT_FALSE(conv.is_empty());
}

TEST(ConversationTest, Accessors) {
    Conversation conv;
    conv.push(Message::user().with_text("hello"));
    conv.push(Message::assistant().with_text("world"));
    EXPECT_EQ(conv.len(), 2u);
    EXPECT_EQ(conv.messages()[0].as_concat_text(), "hello");
    EXPECT_EQ(conv.messages()[1].as_concat_text(), "world");
}

// ========== Message JSON serialization tests ==========

TEST(MessageJsonTest, RoundTrip) {
    Message msg = Message::user().with_text("test message");
    nlohmann::json j;
    to_json(j, msg);

    Message restored;
    from_json(j, restored);

    EXPECT_EQ(restored.role, Role::User);
    EXPECT_EQ(restored.as_concat_text(), "test message");
}

TEST(MessageJsonTest, AssistantWithToolRequest) {
    Message msg = Message::assistant();
    CallToolRequestParams params;
    params.name = "shell";
    params.arguments = {{"command", "ls"}};
    msg.content.push_back(ToolRequest{"tool_1", std::move(params)});

    nlohmann::json j;
    to_json(j, msg);

    Message restored;
    from_json(j, restored);

    EXPECT_EQ(restored.role, Role::Assistant);
    bool has_tool = false;
    for (const auto& block : restored.content) {
        if (std::holds_alternative<ToolRequest>(block)) {
            has_tool = true;
            const auto& tr = std::get<ToolRequest>(block);
            EXPECT_EQ(tr.id, "tool_1");
            if (std::holds_alternative<CallToolRequestParams>(tr.tool_call)) {
                const auto& p = std::get<CallToolRequestParams>(tr.tool_call);
                EXPECT_EQ(p.name, "shell");
                EXPECT_EQ(p.arguments["command"], "ls");
            }
        }
    }
    EXPECT_TRUE(has_tool);
}
