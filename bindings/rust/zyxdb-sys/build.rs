use std::{
    env,
    path::{Path, PathBuf},
};

fn main() {
    println!("cargo:rerun-if-env-changed=ZYX_LIB_DIR");

    if env::var_os("CARGO_FEATURE_SYSTEM").is_none() {
        panic!("zyxdb-sys requires a native library backend. Enable the `system` feature to link against an existing libzyx.");
    }

    let lib_dir = env::var_os("ZYX_LIB_DIR")
        .map(PathBuf::from)
        .or_else(discover_repo_lib_dir);
    if let Some(lib_dir) = lib_dir {
        println!("cargo:rustc-link-search=native={}", lib_dir.display());
        emit_runtime_path(&lib_dir);
    }

    println!("cargo:rustc-link-lib=dylib=zyx");
}

fn emit_runtime_path(lib_dir: &Path) {
    if cfg!(target_os = "macos") || cfg!(target_os = "linux") {
        println!("cargo:rustc-link-arg=-Wl,-rpath,{}", lib_dir.display());
    }
}

fn discover_repo_lib_dir() -> Option<PathBuf> {
    let manifest_dir = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR")?);
    let repo_root = manifest_dir.ancestors().nth(3)?;

    [
        "buildDir",
        "build",
        "cmake-build-debug",
        "cmake-build-release",
        "out/build",
    ]
    .into_iter()
    .map(|dir| repo_root.join(dir))
    .find(|dir| has_zyx_library(dir))
}

fn has_zyx_library(dir: &Path) -> bool {
    [
        "libzyx.dylib",
        "libzyx.so",
        "zyx.dll",
        "zyx.lib",
        "Debug/zyx.dll",
        "Debug/zyx.lib",
        "Release/zyx.dll",
        "Release/zyx.lib",
    ]
    .into_iter()
    .any(|name| dir.join(name).is_file())
}
