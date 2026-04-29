"""Write an explicit MSVC Conan 2 default profile for Windows CI.

Used by cibuildwheel CIBW_BEFORE_ALL_WINDOWS to bypass conan profile
detect, which incorrectly picks up VS-bundled clang-cl on GHA runners.
"""

import pathlib

profile_path = pathlib.Path("conan_msvc_profile")

profile_path.write_text(
    "[settings]\n"
    "os=Windows\n"
    "arch=x86_64\n"
    "build_type=Release\n"
    "compiler=msvc\n"
    "compiler.version=194\n"
    "compiler.cppstd=20\n"
    "compiler.runtime=dynamic\n"
)
