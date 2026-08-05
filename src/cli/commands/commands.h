#pragma once
#include "../cli_parser.h"

namespace goose {
namespace cli {

int run_session_cmd(const CliArgs& args);
int run_run_cmd(const CliArgs& args);
int run_configure_cmd(const CliArgs& args);
int run_info_cmd(const CliArgs& args);
int run_doctor_cmd(const CliArgs& args);
int run_sessions_cmd(const CliArgs& args);
int run_template_cmd(const CliArgs& args);
int run_key_cmd(const CliArgs& args);

}} // namespace goose::cli
