#include <gtest/gtest.h>
#include "../src/extension/mcp_client.h"
#include "../src/extension/extension_config.h"
#include "../src/extension/extension_manager.h"
#include <fstream>
#include <filesystem>

using namespace goose;

TEST(MCPClientTest, ConfigStdio) {
    auto config = ExtensionConfig::stdio("test-ext", "echo", {"hello"});
    EXPECT_EQ(config.name, "test-ext");
    EXPECT_EQ(config.cmd, "echo");
    EXPECT_EQ(config.args.size(), 1u);
    EXPECT_EQ(config.args[0], "hello");
}

TEST(MCPClientTest, ConfigBuiltin) {
    auto config = ExtensionConfig::builtin("developer");
    EXPECT_EQ(config.name, "developer");
    EXPECT_EQ(config.type, ExtensionConfig::Type::Builtin);
}

TEST(MCPClientTest, ConfigStreamableHttp) {
    auto config = ExtensionConfig::streamable_http("web-ext", "http://localhost:8080");
    EXPECT_EQ(config.name, "web-ext");
    EXPECT_EQ(config.uri, "http://localhost:8080");
}

TEST(ExtensionManagerTest, AddBuiltinDeveloper) {
    ExtensionManager mgr;
    auto config = ExtensionConfig::builtin("developer");
    mgr.add_extension(config);
    auto tools = mgr.list_tools();
    ASSERT_TRUE(tools.has_value());
    EXPECT_FALSE(tools->empty());

    bool found_shell = false;
    bool found_write = false;
    bool found_read = false;
    for (const auto& tool : *tools) {
        if (tool.name.find("shell") != std::string::npos) found_shell = true;
        if (tool.name.find("write") != std::string::npos) found_write = true;
        if (tool.name.find("read") != std::string::npos) found_read = true;
    }
    EXPECT_TRUE(found_shell);
    EXPECT_TRUE(found_write);
    EXPECT_TRUE(found_read);
}

TEST(ExtensionManagerTest, CallBuiltinTool) {
    ExtensionManager mgr;
    auto config = ExtensionConfig::builtin("developer");
    mgr.add_extension(config);

    nlohmann::json args = {{"command", "echo test123"}};
    auto result = mgr.call_tool("developer__shell", args);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->dump().find("test123") != std::string::npos);
}

TEST(ExtensionManagerTest, EditWithEmptyBeforeIsRejected) {
    ExtensionManager mgr;
    mgr.add_extension(ExtensionConfig::builtin("developer"));

    auto tmp = std::filesystem::temp_directory_path() / "kacli_edit_test.txt";
    {
        std::ofstream ofs(tmp);
        ofs << "alpha beta\n";
    }

    nlohmann::json args = {
        {"path", tmp.string()},
        {"before", ""},
        {"after", "gamma"}
    };
    auto result = mgr.call_tool("developer__edit", args);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("before"), std::string::npos);

    std::ifstream ifs(tmp);
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "alpha beta\n");
    std::filesystem::remove(tmp);
}

TEST(ExtensionManagerTest, ReadOversizedFileReturnsTruncatedTail) {
    ExtensionManager mgr;
    mgr.add_extension(ExtensionConfig::builtin("developer"));

    auto tmp = std::filesystem::temp_directory_path() / "kacli_read_big.txt";
    {
        std::ofstream ofs(tmp);
        std::string big(2 * 1024 * 1024, 'x');
        ofs << big;
        ofs.flush();
    }

    nlohmann::json args = {{"path", tmp.string()}};
    auto result = mgr.call_tool("developer__read", args);
    ASSERT_TRUE(result.has_value());
    std::string output = result->value("output", "");
    ASSERT_FALSE(output.empty());
    EXPECT_LT(output.size(), 1024u * 1024u + 64u);
    EXPECT_NE(output.find("(truncated)"), std::string::npos);
    std::filesystem::remove(tmp);
}

TEST(ExtensionManagerTest, TreeOutputIsCapped) {
    ExtensionManager mgr;
    mgr.add_extension(ExtensionConfig::builtin("developer"));

    auto dir = std::filesystem::temp_directory_path() / "kacli_tree_cap";
    std::filesystem::create_directories(dir);
    for (int i = 0; i < 2100; ++i) {
        std::ofstream ofs(dir / ("f" + std::to_string(i) + ".txt"));
        ofs << "x";
    }

    nlohmann::json args = {{"path", dir.string()}, {"depth", 1}};
    auto result = mgr.call_tool("developer__tree", args);
    ASSERT_TRUE(result.has_value());
    std::string output = result->value("output", "");
    EXPECT_NE(output.find("truncated"), std::string::npos);
    std::filesystem::remove_all(dir);
}
