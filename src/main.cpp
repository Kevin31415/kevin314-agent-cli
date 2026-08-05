#include "cli/cli_parser.h"
#include "cli/commands/commands.h"
#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>
#include <iostream>
#include <csignal>

#ifndef _WIN32
    #include <signal.h>
#endif

int main(int argc, char* argv[]) {
#ifndef _WIN32
    // MCP 子进程死亡时对 stdin 管道 write 会触发 SIGPIPE；忽略后由
    // 写入路径的 EPIPE 错误正常处理，避免整个进程被信号杀死。
    std::signal(SIGPIPE, SIG_IGN);
#endif
    try {
        auto args = goose::cli::parse_args(argc, argv);

        if (args.debug) {
            spdlog::set_level(spdlog::level::debug);
        } else {
            spdlog::set_level(spdlog::level::info);
        }

        if (args.command == "session") {
            return goose::cli::run_session_cmd(args);
        } else if (args.command == "run") {
            return goose::cli::run_run_cmd(args);
        } else if (args.command == "configure") {
            return goose::cli::run_configure_cmd(args);
        } else if (args.command == "info") {
            return goose::cli::run_info_cmd(args);
        } else if (args.command == "doctor") {
            return goose::cli::run_doctor_cmd(args);
        } else if (args.command == "sessions") {
            return goose::cli::run_sessions_cmd(args);
        } else if (args.command == "template") {
            return goose::cli::run_template_cmd(args);
        } else if (args.command == "key") {
            return goose::cli::run_key_cmd(args);
        } else {
            std::cerr << "未知命令: " << args.command << std::endl;
            return 1;
        }
    } catch (const CLI::ParseError& e) {
        return e.get_exit_code();
    } catch (const std::exception& e) {
        std::cerr << "致命错误: " << e.what() << std::endl;
        return 1;
    }
}
