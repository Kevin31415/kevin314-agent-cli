@echo off
setlocal
rem ============================================================
rem  Windows (x86/x64) 构建脚本 —— 依赖由 conan 管理
rem  在目标 Windows 机器上双击或执行本脚本即可编译出 kacli.exe
rem
rem  前提：已安装 cmake + Visual Studio（MSVC）+ conan
rem    pip install conan
rem  用法：进入 win\ 目录执行 build.bat
rem ============================================================
cd /d "%~dp0"

where conan >nul 2>nul
if errorlevel 1 (
    echo [win] 未找到 conan，请先安装：pip install conan
    exit /b 1
)

where cmake >nul 2>nul
if errorlevel 1 (
    echo [win] 未找到 cmake，请先安装（或通过 Visual Studio Installer / winget install cmake）
    exit /b 1
)

rem 生成默认编译配置（首次；检测 MSVC）
conan profile detect --force >nul 2>nul

rem 安装依赖（conancenter 有预编译包则直接下载，否则本地编译；首次需要网络）
if not exist "build-conan\ftxui-config.cmake" (
    echo [win] conan install（首次需要联网，boost 等可能较慢）...
    conan install . --output-folder=build-conan --build=missing -s build_type=Release
    if errorlevel 1 exit /b 1
)

echo [win] cmake configure ...
cmake -S . -B build ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_PREFIX_PATH=%CD%\build-conan ^
    -Dftxui_DIR=%CD%\build-conan
if errorlevel 1 exit /b 1

echo [win] building target kacli ...
cmake --build build --config Release --target kacli
if errorlevel 1 exit /b 1

echo.
echo Windows 二进制:
if exist "build\src\Release\kacli.exe" (
    echo   %CD%\build\src\Release\kacli.exe
) else if exist "build\src\kacli.exe" (
    echo   %CD%\build\src\kacli.exe
) else (
    echo   build\src\ 下查找 kacli.exe
)

endlocal
