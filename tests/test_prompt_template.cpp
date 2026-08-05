#include <cstdlib>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "prompt_templates_generated.h"
#include "utils/prompt_template.h"
#include "core/prompt_manager.h"
#include "core/tool.h"

using json = nlohmann::json;
using namespace goose;

namespace {

json sample_context() {
    return json{
        {"today", "2026-08-01"},
        {"working_directory", "/home/user/proj"},
        {"mode", "auto"},
        {"extensions", json::array({{
            {"name", "dev"},
            {"description", "developer extension"},
            {"instructions", "Dev instructions"},
            {"env_keys", json::array({"API_KEY"})}
        }})},
        {"tools", json::array({
            {{"name", "shell"}}, {{"name", "developer"}}, {{"name", "computer_controller"}}
        })},
        {"extension_tool_limits", json::array({2, 3})},
        {"extension_count", 1},
        {"tool_count", 3},
        {"conversation_id", "sess-1"},
        {"model", "gpt-oss-120b"},
        {"user_intent", json::array({"Fix the bug"})},
        {"technical_concepts", json::array({"templates"})},
        {"files", json::array({{
            {"path", "src/a.cpp"},
            {"key_code", "int main() {}"},
            {"summary", "entry point"}
        }})},
        {"errors_and_fixes", json::array()},
        {"problem_solving", json::array()},
        {"pending_tasks", json::array({"Test"})},
        {"subagent_id", "sub-42"},
        {"task_instructions", "do the thing"},
        {"user_messages", json::array({{{"role", "user"}, {"content", "hi"}}})},
        {"current_work", "implementing"},
        {"next_step", "test"},
        {"session_name", "my session"}
    };
}

TEST(PromptTemplate, RendersAllEmbeddedTemplates) {
    auto& tmpl = PromptTemplate::global();
    const json ctx = sample_context();
    for (const auto& t : embedded::kEmbeddedTemplates) {
        auto out = tmpl.render(std::string(t.name), ctx);
        EXPECT_FALSE(out.empty()) << "template " << t.name << " rendered empty";
        EXPECT_EQ(out.find("{%"), std::string::npos)
            << "template " << t.name << " has unrendered statement";
        EXPECT_EQ(out.find("{{"), std::string::npos)
            << "template " << t.name << " has unrendered expression";
    }
}

TEST(PromptTemplate, SystemPromptBrandingAndSections) {
    auto& tmpl = PromptTemplate::global();
    auto out = tmpl.render("system.md", sample_context());
    EXPECT_NE(out.find("你是由 kss 团队创建的名为 kacli 的通用 AI Agent。"),
              std::string::npos);
    EXPECT_NE(out.find("## dev"), std::string::npos);
    EXPECT_NE(out.find("Dev instructions"), std::string::npos);
    EXPECT_NE(out.find("# 响应指南"), std::string::npos);
}

TEST(PromptTemplate, IsDefinedResolvedAgainstContext) {
    auto& tmpl = PromptTemplate::global();
    json ctx = sample_context();
    ctx.erase("moim_system_prompt_block");
    auto out = tmpl.render_string(
        "{% if moim_system_prompt_block is defined %}HAS-MOIM{% else %}NO-MOIM{% endif %}",
        ctx);
    EXPECT_EQ(out, "NO-MOIM");
    ctx["moim_system_prompt_block"] = json(nullptr);
    auto out2 = tmpl.render_string(
        "{% if moim_system_prompt_block is defined %}HAS-MOIM{% else %}NO-MOIM{% endif %}",
        ctx);
    EXPECT_EQ(out2, "HAS-MOIM");
}

TEST(PromptTemplate, WithTupleExpansion) {
    auto& tmpl = PromptTemplate::global();
    json ctx = {{"limits", json::array({2, 3})}};
    auto out = tmpl.render_string(
        "{% with (a, b) = limits %}{{a}}/{{b}}{% endwith %}", ctx);
    EXPECT_EQ(out, "2/3");
}

TEST(PromptTemplate, UndefinedVariablesRenderEmpty) {
    auto& tmpl = PromptTemplate::global();
    json ctx;
    EXPECT_EQ(tmpl.render_string("x={{missing}}", ctx), "x=");
    EXPECT_EQ(tmpl.render_string("{% if missing %}T{% else %}F{% endif %}", ctx), "F");
}

TEST(PromptTemplate, MarkdownHeadingsAreTextNotStatements) {
    auto& tmpl = PromptTemplate::global();
    json ctx;
    EXPECT_EQ(tmpl.render_string("## Hello\n{% if true %}ok{% endif %}", ctx), "## Hello\nok");
}

TEST(PromptTemplate, CustomFunctions) {
    auto& tmpl = PromptTemplate::global();
    json ctx = {{"code", "a`b"}};
    EXPECT_EQ(tmpl.render_string("{{ code | code_fence }}", ctx),
              "```\na`b\n```");
    EXPECT_EQ(sanitize_unicode_tags("ab\xf3\xa0\x80\x80""c"), "abc");
}

TEST(PromptTemplate, UserOverride) {
    const std::string tmp_dir = std::string("/tmp/kacli_test_prompts_") +
                                std::to_string(std::rand());
    setenv("GOOSE_CONFIG_DIR", tmp_dir.c_str(), 1);
    auto& tmpl = PromptTemplate::global();
    const std::string marker = "OVERRIDE-MARKER-xyz";
    tmpl.save_template("system.md", marker);
    EXPECT_EQ(tmpl.render("system.md", sample_context()), marker);
    EXPECT_TRUE(tmpl.get_template("system.md").is_customized);
    tmpl.reset_template("system.md");
    EXPECT_FALSE(tmpl.get_template("system.md").is_customized);
    auto out = tmpl.render("system.md", sample_context());
    EXPECT_NE(out.find("kss 团队"), std::string::npos);
    unsetenv("GOOSE_CONFIG_DIR");
}

TEST(PromptTemplate, CodeFenceLongerThanLongestBacktickRun) {
    EXPECT_EQ(code_fence("```x```"), "````\n```x```\n````");
    EXPECT_EQ(code_fence("no ticks"), "```\nno ticks\n```");
}

namespace {
std::vector<Tool> sample_tools() {
    return {
        {"shell", "run shell commands", nlohmann::json::object(), std::nullopt},
        {"read_file", "read a file from disk", nlohmann::json::object(), true},
    };
}
} // namespace

TEST(PromptManager, FullSystemPromptRendersAllVariables) {
    const std::string tmp_dir = std::string("/tmp/kacli_pm_full_") + std::to_string(std::rand());
    setenv("GOOSE_CONFIG_DIR", tmp_dir.c_str(), 1);
    setenv("KACLI_SYSTEM_PROMPT_TEMPLATE", "system.md", 1);

    PromptManager pm;
    auto out = pm.get_system_prompt(sample_tools(), GooseMode::Auto, "/home/test", "Be careful");

    EXPECT_NE(out.find("名为 kacli 的通用 AI Agent"), std::string::npos);
    EXPECT_NE(out.find("## shell"), std::string::npos);
    EXPECT_NE(out.find("run shell commands"), std::string::npos);
    EXPECT_NE(out.find("## 模式：auto"), std::string::npos);
    EXPECT_NE(out.find("## 工作目录"), std::string::npos);
    EXPECT_NE(out.find("/home/test"), std::string::npos);
    EXPECT_NE(out.find("## 当前时间"), std::string::npos);
    EXPECT_NE(out.find("## 额外说明"), std::string::npos);
    EXPECT_NE(out.find("Be careful"), std::string::npos);
    EXPECT_EQ(out.find("{%"), std::string::npos);
    EXPECT_EQ(out.find("{{"), std::string::npos);

    unsetenv("KACLI_SYSTEM_PROMPT_TEMPLATE");
    unsetenv("GOOSE_CONFIG_DIR");
}

TEST(PromptManager, TinySystemPromptRendersAllVariables) {
    const std::string tmp_dir = std::string("/tmp/kacli_pm_tiny_") + std::to_string(std::rand());
    setenv("GOOSE_CONFIG_DIR", tmp_dir.c_str(), 1);
    setenv("KACLI_SYSTEM_PROMPT_TEMPLATE", "tiny_model_system.md", 1);

    PromptManager pm;
    auto out = pm.get_system_prompt(sample_tools(), GooseMode::Auto, "/home/test", "Be careful");

    EXPECT_NE(out.find("你是 kacli，一个由 kss 团队创建的AI Agent"), std::string::npos);
    EXPECT_NE(out.find("操作系统是 "), std::string::npos);
    EXPECT_NE(out.find("工作目录是 /home/test"), std::string::npos);
    EXPECT_EQ(out.find("## Mode"), std::string::npos);
    EXPECT_EQ(out.find("{%"), std::string::npos);
    EXPECT_EQ(out.find("{{"), std::string::npos);

    unsetenv("KACLI_SYSTEM_PROMPT_TEMPLATE");
    unsetenv("GOOSE_CONFIG_DIR");
}

TEST(PromptManager, ToolsAreInjectedIntoSingleSystemPrompt) {
    // 工具列表是系统提示的一部分（Extensions 段），没有单独的工具系统提示。
    const std::string tmp_dir = std::string("/tmp/kacli_pm_tools_") + std::to_string(std::rand());
    setenv("GOOSE_CONFIG_DIR", tmp_dir.c_str(), 1);
    setenv("KACLI_SYSTEM_PROMPT_TEMPLATE", "system.md", 1);

    PromptManager pm;
    auto with_tools = pm.get_system_prompt(sample_tools(), GooseMode::Auto, "/home/test", {});
    auto without = pm.get_system_prompt({}, GooseMode::Auto, "/home/test", {});

    EXPECT_NE(with_tools.find("run shell commands"), std::string::npos);
    EXPECT_EQ(without.find("run shell commands"), std::string::npos);
    EXPECT_NE(without.find("未定义任何扩展"), std::string::npos);

    unsetenv("KACLI_SYSTEM_PROMPT_TEMPLATE");
    unsetenv("GOOSE_CONFIG_DIR");
}

} // namespace
