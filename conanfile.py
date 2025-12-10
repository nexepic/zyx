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
        self.requires("antlr4-cppruntime/4.13.1")

    def configure(self):
        # Set specific dependencies to shared
        self.options["boost"].shared = True
        self.options["zlib"].shared = True
        self.options["gtest"].shared = True

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