#include <gtest/gtest.h>
#include "core/types.h"
#include "core/tool.h"

using namespace goose;

TEST(MessageTest, UserBuilder) {
    Message msg = Message::user().with_text("hello");
    EXPECT_EQ(msg.role, Role::User);
    EXPECT_EQ(msg.as_concat_text(), "hello");
}

TEST(MessageTest, AssistantBuilder) {
    Message msg = Message::assistant().with_text("world");
    EXPECT_EQ(msg.role, Role::Assistant);
    EXPECT_EQ(msg.as_concat_text(), "world");
}

TEST(MessageTest, WithGeneratedId) {
    Message msg = Message::user().with_text("test");
    msg.with_generated_id_if_missing();
    EXPECT_FALSE(msg.id.empty());
}

TEST(ConversationTest, PushAndLen) {
    Conversation conv;
    conv.push(Message::user().with_text("a"));
    conv.push(Message::assistant().with_text("b"));
    EXPECT_EQ(conv.len(), 2u);
    EXPECT_FALSE(conv.is_empty());
}

TEST(ToolTest, Serialize) {
    Tool t{"shell", "run a command", nlohmann::json{{"type", "object"}}};
    nlohmann::json j = t;
    EXPECT_EQ(j["name"], "shell");
    EXPECT_EQ(j["description"], "run a command");
}

TEST(RoleTest, UnknownRoleDefaultsToUser) {
    EXPECT_EQ(role_from_string("user"), Role::User);
    EXPECT_EQ(role_from_string("assistant"), Role::Assistant);
    EXPECT_EQ(role_from_string("system"), Role::User);
    EXPECT_EQ(role_from_string(""), Role::User);
}

TEST(MessageJsonTest, UnknownContentBlockTypeDoesNotThrow) {
    nlohmann::json j;
    j["id"] = "1";
    j["role"] = "assistant";
    j["content"] = nlohmann::json::array();
    j["content"].push_back(nlohmann::json{{"type", "mystery"}, {"data", "x"}});
    j["content"].push_back(nlohmann::json{{"text", "no type field"}});

    Message msg;
    ASSERT_NO_THROW(from_json(j, msg));
    EXPECT_EQ(msg.content.size(), 2u);
    EXPECT_EQ(msg.role, Role::Assistant);
}
