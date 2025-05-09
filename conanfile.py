from conan import ConanFile
import platform

class ProjectConan(ConanFile):
    settings = "os", "compiler", "build_type", "arch"
    generators = "PkgConfigDeps", "MesonToolchain"

    def requirements(self):
        # Common dependencies for all platforms
        self.requires("boost/1.87.0")
        self.requires("zlib/1.2.13")
        self.requires("gtest/1.13.0")

    def configure(self):
        # Set all dependencies to shared
        self.options["boost/*"].shared = True
        self.options["zlib/*"].shared = True
        self.options["gtest/*"].shared = True