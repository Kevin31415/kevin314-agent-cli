#include "cli/cli_parser.h"
#include <CLI/CLI.hpp>

namespace goose {
namespace cli {

CliArgs parse_args(int argc, char* argv[]) {
    CliArgs args;
    CLI::App app{"Kevin314 智能体命令行 (kacli)"};
    app.require_subcommand(0, 1);

    auto* session_cmd = app.add_subcommand("session", "启动或恢复交互式会话");
    session_cmd->alias("s");
    session_cmd->add_flag("--resume", args.resume, "恢复上一次会话");
    session_cmd->add_option("-n,--name", args.name, "会话名称");
    session_cmd->add_option("--session-id", args.session_id, "会话 ID");

    auto* run_cmd = app.add_subcommand("run", "执行一次性指令");
    run_cmd->add_option("-i,--instructions", args.instructions, "指令文件路径");
    run_cmd->add_option("-t,--text", args.text, "输入文本");
    run_cmd->add_option("-r,--recipe", args.recipe, "按配方名称运行");
    run_cmd->add_option("-s,--system", args.system_prompt, "系统提示词文件");
    run_cmd->add_option("--output-format", args.output_format, "输出格式: text|json");
    run_cmd->add_flag("-q,--quiet", args.quiet, "静默模式");

    auto* configure_cmd = app.add_subcommand("configure", "配置 kacli 设置");
    (void)configure_cmd;

    auto* info_cmd = app.add_subcommand("info", "显示 kacli 信息");
    (void)info_cmd;

    auto* doctor_cmd = app.add_subcommand("doctor", "检查 kacli 环境并诊断问题");
    (void)doctor_cmd;

    auto* sessions_cmd = app.add_subcommand("sessions", "列出或管理会话");
    sessions_cmd->alias("ls");
    bool delete_session = false;
    std::string session_name_or_id;
    sessions_cmd->add_flag("-d,--delete", delete_session, "删除会话");
    sessions_cmd->add_option("session", session_name_or_id, "会话名称或 ID");

    auto* template_cmd = app.add_subcommand("template", "管理提示词模板");
    std::string template_name;
    std::string template_content;
    auto* tpl_list = template_cmd->add_subcommand("list", "列出所有模板");
    auto* tpl_show = template_cmd->add_subcommand("show", "显示模板内容");
    tpl_show->add_option("name", template_name, "模板名称")->required();
    auto* tpl_save = template_cmd->add_subcommand("save", "保存自定义模板");
    tpl_save->add_option("name", template_name, "模板名称")->required();
    tpl_save->add_option("content", template_content, "模板内容");
    auto* tpl_reset = template_cmd->add_subcommand("reset", "恢复模板为默认值");
    tpl_reset->add_option("name", template_name, "模板名称")->required();

    auto* key_cmd = app.add_subcommand("key", "查看或配置 API 密钥");
    std::string key_value;
    key_cmd->add_option("value", key_value, "API 密钥值（不带参数则显示当前密钥）");
    key_cmd->add_flag("--unset", args.key_unset, "删除已保存的 API 密钥");

    for (auto* cmd : {session_cmd, run_cmd, configure_cmd, info_cmd, doctor_cmd, sessions_cmd}) {
        cmd->add_option("--provider", args.provider, "提供商名称");
        cmd->add_option("--model", args.model, "模型名称");
        cmd->add_flag("--debug", args.debug, "调试模式");
        cmd->add_option("--max-turns", args.max_turns, "最大轮数");
    }

    for (auto* cmd : {session_cmd, run_cmd}) {
        cmd->add_option("--with-extension", args.extensions, "添加 stdio 扩展");
    }

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        if (dynamic_cast<const CLI::CallForHelp*>(&e)) {
            std::cout << app.help() << std::endl;
        } else if (dynamic_cast<const CLI::CallForVersion*>(&e)) {
            std::cout << app.version() << std::endl;
        } else {
            std::cerr << e.what() << std::endl;
        }
        throw;
    }

    if (run_cmd->parsed()) args.command = "run";
    else if (configure_cmd->parsed()) args.command = "configure";
    else if (info_cmd->parsed()) args.command = "info";
    else if (doctor_cmd->parsed()) args.command = "doctor";
    else if (sessions_cmd->parsed()) {
        args.command = "sessions";
        if (!session_name_or_id.empty()) {
            args.name = session_name_or_id;
        }
        args.session_delete = delete_session;
    }
    else if (template_cmd->parsed()) {
        args.command = "template";
        if (tpl_list->parsed()) args.template_action = "list";
        else if (tpl_show->parsed()) args.template_action = "show";
        else if (tpl_save->parsed()) args.template_action = "save";
        else if (tpl_reset->parsed()) args.template_action = "reset";
        args.template_name = template_name;
        args.template_content = template_content;
    }
    else if (key_cmd->parsed()) {
        args.command = "key";
        if (!key_value.empty()) args.key_value = key_value;
    }
    else args.command = "session";

    return args;
}

}} // namespace goose::cli
