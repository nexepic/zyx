from conan import ConanFile
from conan.tools.meson import MesonToolchain
from conan.tools.gnu import PkgConfigDeps

class ProjectConan(ConanFile):
    settings = "os", "compiler", "build_type", "arch"

    def requirements(self):
        # Common dependencies for all platforms
        self.requires("boost/1.87.0")
        self.requires("zlib/1.3.1")
        self.requires("gtest/1.13.0")
        self.requires("cli11/2.6.0")
        self.requires("antlr4-cppruntime/4.13.2")

    def configure(self):
        # Set specific dependencies to shared
        self.options["boost"].shared = False
        self.options["zlib"].shared = False
        self.options["gtest"].shared = False
        self.options["cli11"].shared = False

        # Disable Boost components we don't use (requires libbacktrace which may fail)
        self.options["boost"].without_stacktrace = True

        # ANTLR usually works best as static for simple linking,
        # but if you prefer shared, you can uncomment below.
        # self.options["antlr4-cppruntime"].shared = True

    def generate(self):
        # Generates PkgConfig .pc files (e.g., buildDir/zlib.pc)
        pc_deps = PkgConfigDeps(self)
        pc_deps.generate()

        # Generates Meson toolchain file (e.g., buildDir/conan_toolchain.meson)
        meson_tc = MesonToolchain(self)
        meson_tc.generate()