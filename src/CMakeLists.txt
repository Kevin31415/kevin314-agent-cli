if(KACLI_STATIC_DEPS)
    # spdlog/fmt 走 header-only，避免 libspdlog.so / libfmt.so
    add_library(kacli_spdlog INTERFACE)
    target_compile_definitions(kacli_spdlog INTERFACE SPDLOG_FMT_EXTERNAL SPDLOG_HEADER_ONLY)
    target_include_directories(kacli_spdlog INTERFACE /usr/include)
    target_link_libraries(kacli_spdlog INTERFACE fmt::fmt-header-only Threads::Threads)
    set(KACLI_SPDLOG_TARGET kacli_spdlog)
    set(KACLI_FMT_TARGET fmt::fmt-header-only)

    # yaml-cpp 配置模式默认给共享库，静态时直接链接 .a
    find_library(KACLI_YAML_STATIC NAMES yaml-cpp)
    if(KACLI_YAML_STATIC)
        set(KACLI_YAML_TARGET ${KACLI_YAML_STATIC})
    else()
        set(KACLI_YAML_TARGET yaml-cpp)
    endif()

    # libcurl 静态链接需要的传递依赖
    find_library(KACLI_LIB_SSL NAMES ssl)
    find_library(KACLI_LIB_CRYPTO NAMES crypto)
    find_library(KACLI_LIB_Z NAMES z)
    find_library(KACLI_LIB_ZSTD NAMES zstd)
    find_library(KACLI_LIB_BROTLIDEC NAMES brotlidec)
    find_library(KACLI_LIB_BROTLICOM NAMES brotlicommon)
    find_library(KACLI_LIB_NGHTTP2 NAMES nghttp2)
    set(KACLI_CURL_DEPS
        ${KACLI_LIB_SSL} ${KACLI_LIB_CRYPTO} ${KACLI_LIB_Z} ${KACLI_LIB_ZSTD}
        ${KACLI_LIB_BROTLIDEC} ${KACLI_LIB_BROTLICOM} ${KACLI_LIB_NGHTTP2})
else()
    set(KACLI_SPDLOG_TARGET spdlog::spdlog)
    set(KACLI_FMT_TARGET fmt::fmt)
    set(KACLI_YAML_TARGET yaml-cpp)
    set(KACLI_CURL_DEPS "")
endif()

# ========== 核心类型库 ==========
set(PROMPT_TEMPLATES_GENERATED
    ${CMAKE_CURRENT_BINARY_DIR}/generated/prompt_templates_generated.h)
file(GLOB PROMPT_TEMPLATE_SOURCES ${CMAKE_SOURCE_DIR}/src/prompts/*.md)
add_custom_command(
    OUTPUT ${PROMPT_TEMPLATES_GENERATED}
    COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/gen_prompts.py
            ${CMAKE_SOURCE_DIR}/src/prompts ${PROMPT_TEMPLATES_GENERATED}
    DEPENDS ${CMAKE_SOURCE_DIR}/scripts/gen_prompts.py ${PROMPT_TEMPLATE_SOURCES}
)
add_custom_target(kacli_generated_templates
    DEPENDS ${PROMPT_TEMPLATES_GENERATED})

add_library(kacli_core STATIC
    core/types.cpp
    core/tool.cpp
    core/prompt_manager.cpp
    core/moim.cpp
    config/paths.cpp
    utils/prompt_template.cpp
)
target_include_directories(kacli_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(kacli_core PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_include_directories(kacli_core SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(kacli_core PUBLIC
    nlohmann_json::nlohmann_json
    ${KACLI_SPDLOG_TARGET}
)
add_dependencies(kacli_core kacli_generated_templates)

# ========== Provider 库 ==========
add_library(kacli_provider STATIC
    provider/base.cpp
    provider/openai.cpp
    provider/anthropic.cpp
    provider/http_client.cpp
    provider/provider_registry.cpp
    provider/model_config.cpp
    provider/model_context.cpp
    provider/provider_usage.cpp
)
target_include_directories(kacli_provider PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kacli_provider PUBLIC
    kacli_core
    kacli_config
    CURL::libcurl
    ${KACLI_CURL_DEPS}
    nlohmann_json::nlohmann_json
)

# ========== Extension (MCP) 库 ==========
add_library(kacli_extension STATIC
    extension/mcp_client.cpp
    extension/extension_manager.cpp
    extension/extension_config.cpp
    extension/mcp_types.cpp
    extension/developer_extension.cpp
)
target_include_directories(kacli_extension PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kacli_extension PUBLIC
    kacli_core
    Boost::filesystem
    ${KACLI_SPDLOG_TARGET}
)

# ========== Session 库 ==========
add_library(kacli_session STATIC
    session/session_manager.cpp
)
target_include_directories(kacli_session PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kacli_session PUBLIC
    kacli_core
    SQLite::SQLite3
)

# ========== Config 库 ==========
add_library(kacli_config STATIC
    config/config.cpp
    config/secrets.cpp
)
target_include_directories(kacli_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kacli_config PUBLIC
    kacli_core
    ${KACLI_YAML_TARGET}
)

# ========== Hints 库 ==========
add_library(kacli_hints STATIC
    hints/gitignore.cpp
    hints/import_files.cpp
    hints/hints.cpp
)
target_include_directories(kacli_hints PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kacli_hints PUBLIC
    kacli_core
    kacli_config
    ${KACLI_SPDLOG_TARGET}
)

# ========== Agent 核心库 ==========
add_library(kacli_agent STATIC
    core/agent.cpp
    core/compaction.cpp
    security/tool_inspection.cpp
    security/permission_manager.cpp
    security/permission_inspector.cpp
    security/security_inspector.cpp
    security/egress_inspector.cpp
    security/repetition_inspector.cpp
)
target_include_directories(kacli_agent PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kacli_agent PUBLIC
    kacli_core
    kacli_provider
    kacli_extension
    kacli_session
    kacli_config
    ${KACLI_YAML_TARGET}
)

# ========== CLI 库 ==========
add_library(kacli_cli STATIC
    cli/cli_parser.cpp
    cli/commands/commands.cpp
    cli/session/cli_session.cpp
    cli/session/session_builder.cpp
    cli/session/input.cpp
    cli/session/output.cpp
    cli/session/terminal_ui.cpp
)
target_include_directories(kacli_cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kacli_cli PUBLIC
    kacli_agent
    CLI11::CLI11
    ${KACLI_FMT_TARGET}
    ftxui::ftxui
)

# ========== CLI 可执行文件 ==========
add_executable(kacli
    main.cpp
)
target_include_directories(kacli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kacli PRIVATE
    kacli_cli
)

if(WIN32)
    target_link_libraries(kacli PRIVATE ws2_32 crypt32)
endif()
