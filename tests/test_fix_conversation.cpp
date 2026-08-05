#include <gtest/gtest.h>
#include "core/types.h"
#include "core/tool.h"
#include <nlohmann/json.hpp>

using namespace goose;

// ============ Conversation tests ============

TEST(Conversation, PushAndLen) {
    Conversation conv;
    conv.push(Message::user().with_text("hello"));
    conv.push(Message::assistant().with_text("world"));
    EXPECT_EQ(conv.len(), 2u);
    EXPECT_FALSE(conv.is_empty());
}

TEST(Conversation, EmptyByDefault) {
    Conversation conv;
    EXPECT_EQ(conv.len(), 0u);
    EXPECT_TRUE(conv.is_empty());
}

TEST(Conversation, FromVector) {
    std::vector<Message> msgs;
    msgs.push_back(Message::user().with_text("a"));
    msgs.push_back(Message::assistant().with_text("b"));
    Conversation conv(std::move(msgs));
    EXPECT_EQ(conv.len(), 2u);
}

// ============ Message tests ============

TEST(Message, HasToolRequests) {
    Message msg = Message::assistant();
    EXPECT_FALSE(msg.has_tool_requests());

    CallToolRequestParams params{"shell", {{"command", "ls"}}};
    msg.content.push_back(ToolRequest{"req_1", std::move(params)});
    EXPECT_TRUE(msg.has_tool_requests());
}

TEST(Message, AsConcatText) {
    Message msg = Message::assistant();
    msg.content.push_back(TextContent{"hello"});
    msg.content.push_back(TextContent{" world"});
    EXPECT_EQ(msg.as_concat_text(), "hello world");
}

// ============ Message JSON serialization tests ============

TEST(MessageSerialization, RoundTrip) {
    Message msg = Message::user().with_text("hello world");
    msg.with_generated_id_if_missing();

    nlohmann::json j;
    to_json(j, msg);

    Message msg2;
    from_json(j, msg2);

    EXPECT_EQ(msg2.role, Role::User);
    EXPECT_EQ(msg2.as_concat_text(), "hello world");
    EXPECT_FALSE(msg2.id.empty());
}

TEST(MessageSerialization, AssistantRoundTrip) {
    Message msg = Message::assistant().with_text("response");
    msg.with_generated_id_if_missing();

    nlohmann::json j;
    to_json(j, msg);

    Message msg2;
    from_json(j, msg2);

    EXPECT_EQ(msg2.role, Role::Assistant);
    EXPECT_EQ(msg2.as_concat_text(), "response");
}

TEST(MessageSerialization, WithToolRequest) {
    Message msg = Message::assistant();
    CallToolRequestParams params{"shell", {{"command", "ls"}}};
    msg.content.push_back(ToolRequest{"req_1", std::move(params)});

    nlohmann::json j;
    to_json(j, msg);

    Message msg2;
    from_json(j, msg2);

    EXPECT_EQ(msg2.role, Role::Assistant);
    bool found_tool_request = false;
    for (const auto& block : msg2.content) {
        if (auto* tr = std::get_if<ToolRequest>(&block)) {
            found_tool_request = true;
            EXPECT_EQ(tr->id, "req_1");
            EXPECT_TRUE(std::holds_alternative<CallToolRequestParams>(tr->tool_call));
        }
    }
    EXPECT_TRUE(found_tool_request);
}

TEST(MessageSerialization, WithToolResponse) {
    Message msg = Message::user();
    CallToolResult result;
    nlohmann::json text_block;
    text_block["type"] = "text";
    text_block["text"] = "output";
    result.content.push_back(std::move(text_block));
    msg.content.push_back(ToolResponse{"req_1", std::move(result)});

    nlohmann::json j;
    to_json(j, msg);

    Message msg2;
    from_json(j, msg2);

    bool found_tool_response = false;
    for (const auto& block : msg2.content) {
        if (auto* tr = std::get_if<ToolResponse>(&block)) {
            found_tool_response = true;
            EXPECT_EQ(tr->id, "req_1");
            EXPECT_TRUE(std::holds_alternative<CallToolResult>(tr->tool_result));
        }
    }
    EXPECT_TRUE(found_tool_response);
}

TEST(ConversationSerialization, RoundTrip) {
    Conversation conv;
    conv.push(Message::user().with_text("hello"));
    conv.push(Message::assistant().with_text("world"));

    nlohmann::json j;
    to_json(j, conv);

    Conversation conv2;
    from_json(j, conv2);

    EXPECT_EQ(conv2.len(), 2u);
    EXPECT_EQ(conv2.messages()[0].as_concat_text(), "hello");
    EXPECT_EQ(conv2.messages()[1].as_concat_text(), "world");
}

// ============ ToolRequest variant tests ============

TEST(ToolRequest, ErrorVariant) {
    ToolRequest tr;
    tr.id = "test";
    tr.tool_call = std::string("error message");

    EXPECT_TRUE(std::holds_alternative<std::string>(tr.tool_call));
    EXPECT_EQ(std::get<std::string>(tr.tool_call), "error message");
}

TEST(ToolRequest, ParamsVariant) {
    ToolRequest tr;
    tr.id = "test";
    CallToolRequestParams params{"tool", {{"arg", "val"}}};
    tr.tool_call = std::move(params);

    EXPECT_TRUE(std::holds_alternative<CallToolRequestParams>(tr.tool_call));
    EXPECT_EQ(std::get<CallToolRequestParams>(tr.tool_call).name, "tool");
}

// ============ fix_conversation tests ============

TEST(FixConversation, EmptyConversation) {
    Conversation conv;
    Conversation fixed = fix_conversation(conv);
    EXPECT_EQ(fixed.len(), 0u);
}

TEST(FixConversation, UserFirstNoChange) {
    Conversation conv;
    conv.push(Message::user().with_text("hello"));
    conv.push(Message::assistant().with_text("world"));
    Conversation fixed = fix_conversation(conv);
    EXPECT_EQ(fixed.len(), 2u);
}

TEST(FixConversation, AssistantFirstGetsFiller) {
    Conversation conv;
    conv.push(Message::assistant().with_text("world"));
    Conversation fixed = fix_conversation(conv);
    EXPECT_EQ(fixed.len(), 2u);
    auto first = fixed.messages().begin();
    EXPECT_EQ(first->role, Role::User);
    EXPECT_TRUE(first->metadata.user_visible == false);
}

TEST(FixConversation, ConsecutiveAssistantMerged) {
    Conversation conv;
    conv.push(Message::user().with_text("a"));
    conv.push(Message::assistant().with_text("b"));
    conv.push(Message::assistant().with_text("c"));
    conv.push(Message::user().with_text("d"));
    Conversation fixed = fix_conversation(conv);
    EXPECT_EQ(fixed.len(), 3u);
    EXPECT_EQ(fixed.messages()[1].as_concat_text(), "bc");
}
