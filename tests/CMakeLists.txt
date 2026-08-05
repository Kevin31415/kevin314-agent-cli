# tests/CMakeLists.txt

# 基础类型测试
add_executable(test_types
    test_types.cpp
)
target_link_libraries(test_types PRIVATE
    kacli_core
    GTest::gtest_main
)
add_test(NAME test_types COMMAND test_types)

# Prompt 模板引擎测试
add_executable(test_prompt_template
    test_prompt_template.cpp
)
target_include_directories(test_prompt_template PRIVATE
    ${CMAKE_BINARY_DIR}/src/generated
)
target_link_libraries(test_prompt_template PRIVATE
    kacli_core
    kacli_config
    kacli_extension
    GTest::gtest_main
)
add_test(NAME test_prompt_template COMMAND test_prompt_template)

# Provider 测试
add_executable(test_provider
    test_provider.cpp
)
target_link_libraries(test_provider PRIVATE
    kacli_provider
    GTest::gtest_main
)
add_test(NAME test_provider COMMAND test_provider)

# fix_conversation 和消息序列化测试
add_executable(test_fix_conversation
    test_fix_conversation.cpp
)
target_link_libraries(test_fix_conversation PRIVATE
    kacli_core
    GTest::gtest_main
)
add_test(NAME test_fix_conversation COMMAND test_fix_conversation)

# SessionManager 测试
add_executable(test_session_manager
    test_session_manager.cpp
)
target_link_libraries(test_session_manager PRIVATE
    kacli_session
    kacli_config
    kacli_core
    GTest::gtest_main
)
add_test(NAME test_session_manager COMMAND test_session_manager)

# Agent E2E 测试
add_executable(test_agent_e2e
    test_agent_e2e.cpp
)
target_link_libraries(test_agent_e2e PRIVATE
    kacli_agent
    kacli_extension
    kacli_config
    GTest::gtest_main
)
add_test(NAME test_agent_e2e COMMAND test_agent_e2e)

# Hints 系统测试
add_executable(test_hints
    test_hints.cpp
)
target_link_libraries(test_hints PRIVATE
    kacli_hints
    kacli_config
    kacli_extension
    kacli_core
    GTest::gtest_main
)
add_test(NAME test_hints COMMAND test_hints)

# Compaction 测试
add_executable(test_compaction
    test_compaction.cpp
)
target_include_directories(test_compaction PRIVATE
    ${CMAKE_BINARY_DIR}/src/generated
)
target_link_libraries(test_compaction PRIVATE
    kacli_agent
    kacli_config
    GTest::gtest_main
)
add_test(NAME test_compaction COMMAND test_compaction)

# MOIM 测试
add_executable(test_moim
    test_moim.cpp
)
target_link_libraries(test_moim PRIVATE
    kacli_core
    GTest::gtest_main
)
add_test(NAME test_moim COMMAND test_moim)

# 工具检查/权限系统测试
add_executable(test_security
    test_security.cpp
)
target_include_directories(test_security PRIVATE
    ${CMAKE_BINARY_DIR}/src/generated
)
target_link_libraries(test_security PRIVATE
    kacli_agent
    kacli_config
    GTest::gtest_main
)
add_test(NAME test_security COMMAND test_security)

# Config 测试
add_executable(test_config
    test_config.cpp
)
target_link_libraries(test_config PRIVATE
    kacli_provider
    kacli_extension
    kacli_config
    kacli_extension
    GTest::gtest_main
)
add_test(NAME test_config COMMAND test_config)

# MCP/Extension 测试
add_executable(test_mcp
    test_mcp.cpp
)
target_link_libraries(test_mcp PRIVATE
    kacli_extension
    kacli_core
    GTest::gtest_main
)
add_test(NAME test_mcp COMMAND test_mcp)

# Comprehensive tests
add_executable(test_comprehensive
    test_comprehensive.cpp
)
target_link_libraries(test_comprehensive PRIVATE
    kacli_cli
    kacli_provider
    kacli_extension
    kacli_session
    kacli_config
    kacli_core
    GTest::gtest_main
)
add_test(NAME test_comprehensive COMMAND test_comprehensive)

# Markdown 渲染测试
add_executable(test_output
    test_output.cpp
)
target_link_libraries(test_output PRIVATE
    kacli_cli
    GTest::gtest_main
)
add_test(NAME test_output COMMAND test_output)

# TUI 工具函数测试
add_executable(test_terminal_ui
    test_terminal_ui.cpp
)
target_link_libraries(test_terminal_ui PRIVATE
    kacli_cli
    GTest::gtest_main
)
add_test(NAME test_terminal_ui COMMAND test_terminal_ui)

# 模型上下文映射测试
add_executable(test_model_context
    test_model_context.cpp
)
target_link_libraries(test_model_context PRIVATE
    kacli_provider
    kacli_config
    kacli_extension
    GTest::gtest_main
)
add_test(NAME test_model_context COMMAND test_model_context)
