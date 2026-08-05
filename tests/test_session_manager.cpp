#include <gtest/gtest.h>
#include "../src/session/session_manager.h"
#include "../src/config/paths.h"
#include <filesystem>

using namespace goose;

class SessionManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        setenv("GOOSE_CONFIG_DIR",
               (std::filesystem::temp_directory_path() / "kacli_sess_test").string().c_str(), 1);
        auto db_path = paths::data_dir() / "test_sessions.db";
        std::filesystem::remove(db_path);
    }
};

TEST_F(SessionManagerTest, CreateSession) {
    auto& mgr = SessionManager::instance();
    auto cwd = std::filesystem::current_path();
    auto result = mgr.create_session(cwd, "test_session");
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->id.empty());
    EXPECT_EQ(result->name, "test_session");
}

TEST_F(SessionManagerTest, GetSession) {
    auto& mgr = SessionManager::instance();
    auto cwd = std::filesystem::current_path();
    auto created = mgr.create_session(cwd, "get_test");
    ASSERT_TRUE(created.has_value());

    auto loaded = mgr.get_session(created->id);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->name, "get_test");
}

TEST_F(SessionManagerTest, ListSessions) {
    auto& mgr = SessionManager::instance();
    auto cwd = std::filesystem::current_path();
    mgr.create_session(cwd, "list_test1");
    mgr.create_session(cwd, "list_test2");

    auto sessions = mgr.list_sessions();
    ASSERT_TRUE(sessions.has_value());
    EXPECT_GE(sessions->size(), 2u);
}

TEST_F(SessionManagerTest, DeleteSession) {
    auto& mgr = SessionManager::instance();
    auto cwd = std::filesystem::current_path();
    auto created = mgr.create_session(cwd, "delete_test");
    ASSERT_TRUE(created.has_value());

    auto del = mgr.delete_session(created->id);
    ASSERT_TRUE(del.has_value());

    auto loaded = mgr.get_session(created->id);
    EXPECT_FALSE(loaded.has_value());
}

TEST_F(SessionManagerTest, AddMessage) {
    auto& mgr = SessionManager::instance();
    auto cwd = std::filesystem::current_path();
    auto created = mgr.create_session(cwd, "msg_test");
    ASSERT_TRUE(created.has_value());

    Message msg = Message::user().with_text("hello");
    auto add = mgr.add_message(created->id, msg);
    ASSERT_TRUE(add.has_value());

    auto loaded = mgr.get_session(created->id, true);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_TRUE(loaded->conversation.has_value());
    EXPECT_EQ(loaded->conversation->len(), 1u);
}
