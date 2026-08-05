#!/usr/bin/env bash
# 开发构建脚本：最快 Debug 版本，产物在 build/src/kacli
# 用法: ./build-scripts/build.sh
set -euo pipefail

cd "$(dirname "$0")/.."

BUILD_DIR="${BUILD_DIR:-build}"
CONAN_DIR="build-conan"
GENERATORS="$CONAN_DIR/build-conan/Debug/generators"

JOBS="$(nproc 2>/dev/null || echo 4)"

# 首次构建时用 conan 生成 FTXUI 的 Debug 配置（已存在则跳过）
if [ ! -f "$GENERATORS/ftxui-config.cmake" ]; then
    echo "[build] conan install (Debug) -> $CONAN_DIR ..."
    conan install . --output-folder="$CONAN_DIR" --build=missing -s build_type=Debug
fi

echo "[build] cmake configure (Debug) -> $BUILD_DIR ..."
# 若缓存中的 ftxui_DIR 不是本次要用的生成目录（例如上次是 Release），先清理
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    cached_ftxui_dir="$(grep -m1 '^ftxui_DIR:PATH=' "$BUILD_DIR/CMakeCache.txt" || true)"
    if [ -n "$cached_ftxui_dir" ] && [ "$cached_ftxui_dir" != "ftxui_DIR:PATH=$PWD/$GENERATORS" ]; then
        echo "[build] 检测到缓存 ftxui_DIR 与其他构建不一致，清理 $BUILD_DIR ..."
        rm -rf "$BUILD_DIR"
    fi
fi
cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_PREFIX_PATH="$PWD/$GENERATORS" \
    -Dftxui_DIR="$PWD/$GENERATORS"

echo "[build] compiling target kacli ..."
cmake --build "$BUILD_DIR" -j"$JOBS" --target kacli

echo
echo "Debug 二进制: $BUILD_DIR/src/kacli"
