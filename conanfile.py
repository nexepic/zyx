from conan import ConanFile
from conan.tools.cmake import CMakeDeps, CMakeToolchain


class ProjectConan(ConanFile):
    settings = "os", "compiler", "build_type", "arch"
    options = {"with_tests": [True, False]}
    default_options = {"with_tests": True}

    def requirements(self):
        self.requires("boost/1.87.0")
        self.requires("zlib/1.3.1")
        self.requires("cli11/2.6.0")
        # ANTLR4 exposes STL types in public headers. Always build from source
        # with this project's C++ standard to avoid ABI mismatches.
        self.requires("antlr4-cppruntime/4.13.2")
        if self.options.with_tests:
            self.requires("gtest/1.13.0")

    def configure(self):
        self.options["boost"].shared = False
        self.options["zlib"].shared = False
        self.options["cli11"].shared = False
        if self.options.with_tests:
            self.options["gtest"].shared = False

        self.options["boost"].without_stacktrace = True

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()

        toolchain = CMakeToolchain(self)
        toolchain.variables["CMAKE_CXX_STANDARD"] = "20"
        toolchain.variables["CMAKE_CXX_STANDARD_REQUIRED"] = "ON"
        toolchain.variables["ZYX_BUILD_TESTS"] = "ON" if self.options.with_tests else "OFF"
        toolchain.generate()
