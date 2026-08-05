#!/usr/bin/env bash
# Release 构建脚本：Release 版 + 静态链接所有第三方依赖。
# 产出自包含单文件（仅依赖 libc/libm/libgcc_s/libstdc++ 等系统通用库），
# 目标机器无需额外安装运行时依赖。
#
# 说明：系统 libcurl 的静态库依赖 krb5/gssapi/ldap 等（Debian 系无对应 .a，
# 无法静态链接），因此本脚本会先编译一个最小功能的自定义静态 libcurl
# （仅 HTTP/HTTPS + OpenSSL/zlib/zstd/brotli/nghttp2），缓存于
# $BUILD_DIR/_curl/ 下；首次构建需要联网下载 curl 源码。
#
# 用法: ./build-scripts/release.sh
set -euo pipefail

cd "$(dirname "$0")/.."

BUILD_DIR="${BUILD_DIR:-release}"
CONAN_DIR="build-conan"
GENERATORS="$CONAN_DIR/build-conan/Release/generators"
JOBS="$(nproc 2>/dev/null || echo 4)"

CURL_VERSION="8.14.1"
CURL_ROOT="$BUILD_DIR/_curl"
CURL_SRC="$CURL_ROOT/curl-$CURL_VERSION"
CURL_PREFIX="$CURL_ROOT/prefix"
CURL_TARBALL="$CURL_ROOT/curl-$CURL_VERSION.tar.gz"
CURL_PREFIX_ABS="$PWD/$CURL_PREFIX"

# 1) 用 conan 生成 FTXUI 的 Release 配置（已存在则跳过）
if [ ! -f "$GENERATORS/ftxui-config.cmake" ]; then
    echo "[release] conan install -> $CONAN_DIR ..."
    conan install . --output-folder="$CONAN_DIR" --build=missing
fi

# 2) 编译最小静态 libcurl（已存在则跳过）
if [ ! -f "$CURL_PREFIX/lib/libcurl.a" ]; then
    echo "[release] 准备自定义静态 libcurl ($CURL_VERSION) ..."
    mkdir -p "$CURL_ROOT"
    if [ ! -f "$CURL_TARBALL" ]; then
        echo "[release] 下载 curl 源码（首次需要联网）..."
        curl -L --fail -o "$CURL_TARBALL" "https://curl.se/download/curl-$CURL_VERSION.tar.gz"
    fi
    rm -rf "$CURL_SRC"
    tar xzf "$CURL_TARBALL" -C "$CURL_ROOT"
    (
        cd "$CURL_SRC"
        ./configure \
            --disable-shared --enable-static \
            --prefix="$CURL_PREFIX_ABS" \
            --with-openssl --with-zlib --with-zstd --with-brotli --with-nghttp2 \
            --without-nghttp3 --without-ngtcp2 --without-libidn2 --without-libpsl \
            --without-libssh2 --without-librtmp --without-libgsasl \
            --disable-ldap --disable-ldaps --disable-rtsp --disable-ftp --disable-tftp \
            --disable-telnet --disable-smb --disable-imap --disable-pop3 --disable-smtp \
            --disable-gopher --disable-mqtt --disable-dict --disable-file --disable-cookies \
            --disable-gssapi --disable-manual --disable-threaded-resolver \
            --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt --enable-http
        make -j"$JOBS"
        make install
    )
    echo "[release] 自定义静态 libcurl 完成: $CURL_PREFIX/lib/libcurl.a"
fi

echo "[release] cmake configure (Release + KACLI_STATIC_DEPS=ON) -> $BUILD_DIR ..."
# 若缓存中的 ftxui_DIR 不是本次要用的生成目录，先清理避免用错配置
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    cached_ftxui_dir="$(grep -m1 '^ftxui_DIR:PATH=' "$BUILD_DIR/CMakeCache.txt" || true)"
    if [ -n "$cached_ftxui_dir" ] && [ "$cached_ftxui_dir" != "ftxui_DIR:PATH=$PWD/$GENERATORS" ]; then
        echo "[release] 检测到缓存 ftxui_DIR 与其他构建不一致，清理 $BUILD_DIR ..."
        rm -rf "$BUILD_DIR"
    fi
fi
cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DKACLI_STATIC_DEPS=ON \
    -DCMAKE_PREFIX_PATH="$CURL_PREFIX_ABS;$PWD/$GENERATORS" \
    -Dftxui_DIR="$PWD/$GENERATORS" \
    -DCURL_INCLUDE_DIR="$CURL_PREFIX_ABS/include" \
    -DCURL_LIBRARY_RELEASE="$CURL_PREFIX_ABS/lib/libcurl.a"

echo "[release] compiling target kacli ..."
cmake --build "$BUILD_DIR" -j"$JOBS" --target kacli

BIN="$BUILD_DIR/src/kacli"

echo
echo "Release 二进制: $BIN"
echo "校验运行时依赖（应只有系统通用库，无第三方 .so）:"
ldd "$BIN" 2>/dev/null || true
