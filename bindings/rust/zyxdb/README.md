# zyxdb

Rust bindings for the ZYX embedded graph database.

This crate uses the stable ZYX Driver ABI provided by `zyxdb-sys`.
By default, `zyxdb-sys` builds the bundled ZYX native source from the `zyx-src` crate, so users do not need to
install `libzyx` separately.

To link against an existing native `libzyx` instead, disable default features and enable `system`:

```toml
zyxdb = { version = "0.1.24", default-features = false, features = ["system"] }
```

Set `ZYX_LIB_DIR` to the directory containing `libzyx` when it is not in a standard linker path.
Bundled builds require CMake, a C++20 compiler, and ZYX's native dependencies. Set `ZYXDB_SYS_USE_CONAN=1`
to let the build script run Conan for those native dependencies.
