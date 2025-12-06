from conan import ConanFile
from conan.tools.meson import MesonToolchain
from conan.tools.gnu import PkgConfigDeps
# from conan.tools.layout import basic_layout # Consider for structuring build outputs

class ProjectConan(ConanFile):
    settings = "os", "compiler", "build_type", "arch"
    # The 'generators' attribute is not used this way in Conan 2.x for these tools.
    # They are invoked in the generate() method.

    def requirements(self):
        # Common dependencies for all platforms
        self.requires("boost/1.87.0")
        self.requires("zlib/1.3.1")
        self.requires("gtest/1.13.0")

    def configure(self):
        # Set specific dependencies to shared using correct Conan 2.x syntax
        self.options["boost"].shared = True
        self.options["zlib"].shared = True
        self.options["gtest"].shared = True
        # To make all dependencies (direct and transitive) shared, you could use:
        # self.options["*:shared"] = True
        # However, this can be too broad; explicit is often better.

    # def layout(self):
    # basic_layout(self) # This would change output paths, e.g. to build/Debug/generators
    # For your current CI script expecting generated files in 'buildDir',
    # not defining a layout or using a custom one that fits is appropriate.
    # By default, generators output to the folder specified in `conan install --output-folder`
    # if no layout defining `self.folders.generators` is used.

    def generate(self):
        # Generates PkgConfig .pc files (e.g., buildDir/zlib.pc)
        pc_deps = PkgConfigDeps(self)
        pc_deps.generate()

        # Generates Meson toolchain file (e.g., buildDir/conan_toolchain.meson)
        meson_tc = MesonToolchain(self)
        meson_tc.generate()