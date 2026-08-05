#!/usr/bin/env bash
# Termux (Android ARM64) 构建脚本
# 在目标手机上运行本脚本即可编译出 arm64 版 kacli。
#
# 说明：
#   - 依赖全部来自 Termux 官方仓库（pkg），无需 conan。
#   - FTXUI 由 Termux 的 libftxui 包提供（与本项目要求的 6.1.9 一致）。
#   - 产物为动态链接 Termux 共享库的 ARM64 ELF，直接运行于 Termux。
#
# 用法: 把本 termux/ 目录整个拷到手机上（或 git clone 后进入 termux/），然后
#   bash build.sh
set -euo pipefail

cd "$(dirname "$0")"

# 1) 安装依赖（已装会自动跳过；需要网络）
#    boost 较大，若只想尽快编译可改为 boost-headers（但需要 find_package(Boost) 能找到）。
echo "[termux] 安装依赖 ..."
pkg install -y \
  clang cmake ninja make pkg-config python \
  fmt spdlog nlohmann-json cli11 yaml-cpp \
  curl sqlite boost libftxui googletest

# 2) 配置
echo "[termux] cmake configure (Release) ..."
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON

# 3) 编译 kacli（不编译测试，加快速度）
echo "[termux] compiling target kacli ..."
cmake --build build -j"$(nproc)" --target kacli

BIN="build/src/kacli"
echo
echo "Termux (arm64) 二进制: $BIN"
file "$BIN" 2>/dev/null || true
