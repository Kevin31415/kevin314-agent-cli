from conan import ConanFile
from conan.tools.cmake import CMake, cmake_layout, CMakeDeps, CMakeToolchain
from conan.tools.files import copy, collect_libs
import os


class KacliConan(ConanFile):
    name = "kacli"
    version = "0.1.0"
    description = "Kevin314 Agent Cli - C++ CLI implementation"
    settings = "os", "compiler", "build_type", "arch"
    exports_sources = "CMakeLists.txt", "src/*", "tests/*"
    no_copy_source = False

    def requirements(self):
        # 终端 UI (FTXUI) — 其余依赖使用系统包，避免 conan 全量源码编译
        self.requires("ftxui/6.1.9")

    def layout(self):
        cmake_layout(self, src_folder=".", build_folder="build-conan")

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
