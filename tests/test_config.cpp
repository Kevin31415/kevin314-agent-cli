#include <gtest/gtest.h>
#include "../src/config/config.h"
#include "../src/config/paths.h"
#include "../src/provider/provider_registry.h"
#include <yaml-cpp/yaml.h>
#include <fstream>
#include <filesystem>

using namespace goose;

class ConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        setenv("GOOSE_CONFIG_DIR",
               (std::filesystem::temp_directory_path() / "kacli_cfg_test").string().c_str(), 1);
        auto config_path = paths::config_file();
        std::filesystem::create_directories(config_path.parent_path());

        YAML::Node node;
        node["active_provider"] = "openai";
        node["KACLI_MODE"] = "auto";

        node["providers"]["openai"]["enabled"] = true;
        node["providers"]["openai"]["model"] = "gpt-4o";
        node["providers"]["openai"]["configured"] = true;

        node["extensions"]["developer"]["enabled"] = true;
        node["extensions"]["developer"]["type"] = "builtin";
        node["extensions"]["developer"]["name"] = "Developer";

        std::ofstream ofs(config_path);
        ofs << node;
    }
};

TEST_F(ConfigTest, LoadConfig) {
    auto& cfg = Config::global();
    auto provider = cfg.get_active_provider();
    EXPECT_EQ(provider, "openai");
}

TEST_F(ConfigTest, GetModel) {
    auto& cfg = Config::global();
    auto model = cfg.get_active_model();
    EXPECT_EQ(model, "gpt-4o");
}

TEST_F(ConfigTest, GetProviderEntry) {
    auto& cfg = Config::global();
    auto entry = cfg.get_provider("openai");
    ASSERT_TRUE(entry.has_value());
    EXPECT_TRUE(entry->enabled);
    EXPECT_EQ(entry->model, "gpt-4o");
    EXPECT_TRUE(entry->configured);
}

TEST_F(ConfigTest, GetExtensions) {
    auto& cfg = Config::global();
    auto exts = cfg.get_extensions();
    EXPECT_GE(exts.size(), 1u);
    EXPECT_EQ(exts[0].name, "Developer");
}

TEST_F(ConfigTest, SetParam) {
    auto& cfg = Config::global();
    cfg.set_param("KACLI_MODE", "chat");
    auto result = cfg.get_param("KACLI_MODE");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "chat");
}

TEST_F(ConfigTest, GooseMode) {
    auto& cfg = Config::global();
    cfg.set_goose_mode(GooseMode::Approve);
    EXPECT_EQ(cfg.get_goose_mode(), GooseMode::Approve);
}

TEST(GooseModeFromStringTest, UnknownModeYieldsNullopt) {
    EXPECT_FALSE(goose_mode_from_string("definitely_not_a_mode").has_value());
    EXPECT_FALSE(goose_mode_from_string("").has_value());
    EXPECT_EQ(goose_mode_from_string("auto"), GooseMode::Auto);
    EXPECT_EQ(goose_mode_from_string("smart_approve"), GooseMode::SmartApprove);
}

TEST(GooseModeFromStringTest, UnknownEnvModeFallsBackToApprove) {
    setenv("KACLI_MODE", "typo_mode", 1);
    setenv("GOOSE_MODE", "", 1);
    setenv("MODE", "", 1);
    EXPECT_EQ(Config::global().get_goose_mode(), GooseMode::Approve);
    unsetenv("KACLI_MODE");
    unsetenv("GOOSE_MODE");
    unsetenv("MODE");
}

TEST(ProviderRegistryTest, UnknownProviderIsRejected) {
    auto provider = ProviderRegistry::instance().create("definitely_not_a_provider");
    EXPECT_FALSE(provider);
}
