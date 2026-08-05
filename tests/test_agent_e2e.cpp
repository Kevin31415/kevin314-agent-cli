#include <gtest/gtest.h>
#include "../src/core/agent.h"
#include "../src/core/types.h"
#include "../src/provider/base.h"
#include "../src/extension/extension_manager.h"
#include <memory>

using namespace goose;

class MockProvider : public Provider {
public:
    std::string name_ = "mock";

    const std::string& get_name() const override { return name_; }

    Result<MessageStream> stream(
        const ModelConfig&,
        const std::string&,
        const std::vector<Message>& messages,
        const std::vector<Tool>&) override {

        received_messages_.push_back(messages);
        int call_count = ++stream_calls_;
        Message response = Message::assistant();

        if (call_count == 1 && !tool_error_.empty()) {
            ToolRequest tr;
            tr.id = "mock_incomplete_1";
            tr.tool_call = tool_error_;
            response.content.push_back(std::move(tr));
        } else if (call_count == 1 && !tool_response_text_.empty()) {
            ToolRequest tr;
            tr.id = "mock_tool_1";
            CallToolRequestParams params;
            params.name = "developer__shell";
            params.arguments = nlohmann::json{{"command", "echo hello"}};
            tr.tool_call = std::move(params);
            response.content.push_back(std::move(tr));
        } else {
            response.with_text(final_text_);
        }

        response.with_generated_id_if_missing();

        auto done = std::make_shared<bool>(false);
        MessageStream fn = [response, done]() mutable -> Result<StreamChunk> {
            if (*done) {
                return Result<StreamChunk>::err(Error{ErrorCode::StreamEnd, "done"});
            }
            *done = true;
            StreamChunk chunk;
            chunk.message = response;
            ProviderUsage pu;
            pu.provider = "mock";
            pu.usage = Usage{10, 20, std::nullopt, std::nullopt};
            chunk.usage = pu;
            return Result<StreamChunk>::ok(std::move(chunk));
        };

        return Result<MessageStream>::ok(std::move(fn));
    }

    void set_tool_response(const std::string& text) { tool_response_text_ = text; }
    void set_tool_error(const std::string& text) { tool_error_ = text; }
    void set_final_text(const std::string& text) { final_text_ = text; }
    int stream_calls() const { return stream_calls_; }
    const std::vector<std::vector<Message>>& received_messages() const { return received_messages_; }

private:
    int stream_calls_ = 0;
    std::vector<std::vector<Message>> received_messages_;
    std::string tool_response_text_;
    std::string tool_error_;
    std::string final_text_ = "Mock response";
};

class AgentE2ETest : public ::testing::Test {
protected:
    void SetUp() override {
        provider = std::make_shared<MockProvider>();
        ext_mgr = std::make_unique<ExtensionManager>();
        config.session_manager = nullptr;
        agent = std::make_unique<Agent>(provider, config, ext_mgr.get());
    }

    std::shared_ptr<MockProvider> provider;
    std::unique_ptr<ExtensionManager> ext_mgr;
    AgentConfig config;
    std::unique_ptr<Agent> agent;
};

TEST_F(AgentE2ETest, SimpleReply) {
    provider->set_final_text("Hello from mock!");
    provider->set_tool_response("");

    Conversation history;
    history.push(Message::user().with_text("hi"));
    SessionConfig sess{std::nullopt, nullptr};

    std::vector<AgentEvent> events;
    auto result = agent->reply(sess,
        [&](AgentEvent e) { events.push_back(std::move(e)); },
        std::move(history));

    EXPECT_TRUE(result.has_value());
    EXPECT_GE(events.size(), 1u);
    EXPECT_EQ(provider->stream_calls(), 1);
}

TEST_F(AgentE2ETest, UserMessageReachesProvider) {
    provider->set_final_text("Hello from mock!");

    Conversation history;
    history.push(Message::user().with_text("hello provider"));
    SessionConfig sess{std::nullopt, nullptr};

    auto result = agent->reply(sess,
        [](AgentEvent) {}, std::move(history));

    EXPECT_TRUE(result.has_value());
    ASSERT_FALSE(provider->received_messages().empty());
    ASSERT_FALSE(provider->received_messages().back().empty());
    std::string first_user_text = provider->received_messages().back()[0].as_concat_text();
    EXPECT_NE(first_user_text.find("hello provider"), std::string::npos);
    EXPECT_NE(first_user_text.find("<turn-context>"), std::string::npos);
}

TEST_F(AgentE2ETest, ToolCallLoop) {
    provider->set_tool_response("tool called");
    provider->set_final_text("Done after tool");

    Conversation history;
    history.push(Message::user().with_text("do something"));
    SessionConfig sess{10, {}};

    std::vector<AgentEvent> events;
    auto result = agent->reply(sess,
        [&](AgentEvent e) { events.push_back(std::move(e)); },
        std::move(history));

    EXPECT_TRUE(result.has_value());
    EXPECT_GE(provider->stream_calls(), 2);
}

TEST_F(AgentE2ETest, ToolMessagesEmittedOnce) {
    provider->set_tool_response("tool called");
    provider->set_final_text("Done");

    Conversation history;
    history.push(Message::user().with_text("do it"));
    SessionConfig sess{10, {}};

    std::vector<AgentEvent> events;
    auto result = agent->reply(sess,
        [&](AgentEvent e) { events.push_back(std::move(e)); },
        std::move(history));

    EXPECT_TRUE(result.has_value());

    int assistant_tool_msgs = 0;
    int tool_response_msgs = 0;
    for (const auto& e : events) {
        if (e.type != AgentEventType::Message || !e.msg) continue;
        if (e.msg->has_tool_requests()) assistant_tool_msgs++;
        for (const auto& block : e.msg->content) {
            if (std::holds_alternative<ToolResponse>(block)) tool_response_msgs++;
        }
    }
    EXPECT_EQ(assistant_tool_msgs, 1);
    EXPECT_EQ(tool_response_msgs, 1);
}

TEST_F(AgentE2ETest, IncompleteToolCallFeedback) {
    provider->set_tool_error("stream ended before tool call id arrived");
    provider->set_final_text("Done");

    Conversation history;
    history.push(Message::user().with_text("do it"));
    SessionConfig sess{10, {}};

    std::vector<AgentEvent> events;
    auto result = agent->reply(sess,
        [&](AgentEvent e) { events.push_back(std::move(e)); },
        std::move(history));

    EXPECT_TRUE(result.has_value());
    ASSERT_GE(provider->received_messages().size(), 2u);

    const auto& second_call = provider->received_messages()[1];
    bool saw_feedback = false;
    for (const auto& msg : second_call) {
        if (msg.role == Role::User &&
            msg.as_concat_text().find("stream ended before tool call id arrived") != std::string::npos) {
            saw_feedback = true;
        }
    }
    EXPECT_TRUE(saw_feedback);
}

TEST_F(AgentE2ETest, MaxTurnsLimit) {
    provider->set_tool_response("tool");
    provider->set_final_text("Done");

    Conversation history;
    history.push(Message::user().with_text("loop test"));
    SessionConfig sess{2, {}};

    std::vector<AgentEvent> events;
    auto result = agent->reply(sess,
        [&](AgentEvent e) { events.push_back(std::move(e)); },
        std::move(history));

    EXPECT_TRUE(result.has_value());
    EXPECT_LE(provider->stream_calls(), 3);
}

TEST_F(AgentE2ETest, ListTools) {
    auto tools = agent->list_tools();
    EXPECT_TRUE(tools.empty());
}
