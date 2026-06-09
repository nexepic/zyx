# zyxdb-sys

Raw Rust FFI bindings for the ZYX Driver ABI.

This crate exposes the C-facing `zyx_driver_*` API for higher-level Rust crates.
By default, the crate builds the bundled ZYX native source from the `zyx-src` crate and links the resulting `libzyx`.

To link against an existing native `libzyx` instead, disable default features and enable `system`:

```toml
zyxdb-sys = { version = "0.1.24", default-features = false, features = ["system"] }
```

Set `ZYX_LIB_DIR` to the directory containing `libzyx` when it is not in a standard linker path.
Bundled builds require CMake, a C++20 compiler, and ZYX's native dependencies. Set `ZYXDB_SYS_USE_CONAN=1`
to let the build script run Conan for those native dependencies.
