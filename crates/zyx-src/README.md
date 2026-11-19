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

All source builds, including core-only and WASM builds, require the header-only
Boost.Container component for compact query-local containers. The project's
Conan recipe supplies it; system dependency builds must make the headers
available to CMake. `ZYX_BOOST_HEADER_DIR` may point to the directory containing
`boost/`. No Boost runtime library is added to the core by this requirement.
