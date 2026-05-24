# zyxdb

Rust bindings for the ZYX embedded graph database.

This crate uses the stable ZYX Driver ABI provided by `zyxdb-sys`.
The current supported packaging mode links against an existing native `libzyx`.
Set `ZYX_LIB_DIR` to the directory containing `libzyx` when it is not in a standard linker path.
