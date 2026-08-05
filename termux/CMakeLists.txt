cmake_minimum_required(VERSION 3.20)
project(kacli
    VERSION 0.1.0
    DESCRIPTION "Kevin314 Agent Cli - C++ CLI implementation"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ========== 静态单文件构建选项 ==========
# KACLI_STATIC_DEPS=ON 时静态链接所有第三方依赖，产出自包含单文件，
# 仅保留 libc/libm/libgcc_s/libstdc++ 等通用动态依赖。
option(KACLI_STATIC_DEPS
    "Statically link all third-party deps for a self-contained single binary" OFF)
if(KACLI_STATIC_DEPS)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
    set(CMAKE_FIND_LIBRARY_PREFIXES "lib")
    add_link_options(-Wl,--start-group -Wl,--end-group)
endif()

# 查找系统包
find_package(nlohmann_json REQUIRED)
find_package(spdlog REQUIRED)
find_package(CLI11 REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Boost REQUIRED COMPONENTS system filesystem)
find_package(fmt REQUIRED)
find_package(GTest REQUIRED)

# FTXUI (conan 提供，见 build-conan/ 的 conan install)
find_package(ftxui QUIET CONFIG)
if(NOT ftxui_FOUND)
    message(FATAL_ERROR "ftxui not found. Run: "
        "conan install . --output-folder=build-conan --build=missing "
        "then configure with -DCMAKE_PREFIX_PATH=<build-conan>/build-conan/Release/generators")
endif()

# 编译选项
if(MSVC)
    add_compile_options(/W4 /WX- /EHsc)
    add_compile_definitions(
        _CRT_SECURE_NO_WARNINGS
        _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

add_subdirectory(src)
enable_testing()
add_subdirectory(tests)
