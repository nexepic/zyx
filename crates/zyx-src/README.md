<p align="center">
  <a href="https://github.com/nexepic/zyx">
    <img
      src="https://raw.githubusercontent.com/nexepic/zyx/main/assets/branding/icon.svg"
      alt="ZYX logo"
      width="120"
    />
  </a>
</p>

# zyx-src

Vendored C++ source for the ZYX embedded graph database.

This crate is consumed by `zyxdb-sys` to build ZYX from source. Most applications should depend on `zyxdb` instead.

MSVC source builds require Boost.Container headers, including core-only builds.
The project's Conan recipe supplies them; system dependency builds must make
the headers discoverable to CMake. No Boost runtime library is added to the
core by this requirement. Linux, macOS and WASM core-only builds do not require
Boost.Container.
