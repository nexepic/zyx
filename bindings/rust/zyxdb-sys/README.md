# zyxdb-sys

Raw Rust FFI bindings for the ZYX Driver ABI.

This crate exposes the C-facing `zyx_driver_*` API for higher-level Rust crates.
The current supported packaging mode links dynamically against an existing native `libzyx`.
Set `ZYX_LIB_DIR` to the directory containing `libzyx` when it is not in a standard linker path.
