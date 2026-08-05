from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMakeDeps


class KacliWinConan(ConanFile):
    name = "kacli-win"
    version = "0.1.0"
    description = "Kevin314 Agent Cli - Windows build (conan deps)"
    settings = "os", "compiler", "build_type", "arch"
    exports_sources = "CMakeLists.txt", "src/*", "tests/*"

    def requirements(self):
        # Windows 无系统包，全部依赖由 conan 提供。
        # 版本范围避免 conancenter 具体小版本缺失导致安装失败。
        self.requires("nlohmann_json/3.11.3")
        self.requires("spdlog/1.15.3")
        self.requires("cli11/2.6.2")
        self.requires("yaml-cpp/0.8.0")
        self.requires("libcurl/[>=8.9 <9]")
        self.requires("sqlite3/[>=3.46 <4]")
        self.requires("boost/[>=1.86 <2]")
        self.requires("fmt/11.1.2")
        self.requires("gtest/1.15.0")
        self.requires("ftxui/6.1.9")

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
        tc.generate()
