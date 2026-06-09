use std::{
    env, fs,
    path::{Path, PathBuf},
};

fn main() {
    println!("cargo:rerun-if-env-changed=ZYX_LIB_DIR");

    let lib_dir = env::var_os("DEP_ZYX_LIB_DIR")
        .map(PathBuf::from)
        .or_else(|| env::var_os("ZYX_LIB_DIR").map(PathBuf::from))
        .or_else(discover_repo_lib_dir);
    if let Some(lib_dir) = lib_dir {
        emit_runtime_path(&lib_dir);
    }

    if cfg!(target_os = "windows") {
        if let Some(bin_dir) = env::var_os("DEP_ZYX_BIN_DIR").map(PathBuf::from) {
            copy_windows_runtime_dll(&bin_dir);
        }
    }
}

fn emit_runtime_path(lib_dir: &Path) {
    if cfg!(target_os = "macos") || cfg!(target_os = "linux") {
        println!("cargo:rustc-link-arg=-Wl,-rpath,{}", lib_dir.display());
    }
}

fn copy_windows_runtime_dll(bin_dir: &Path) {
    let dll = bin_dir.join("zyx.dll");
    if !dll.exists() {
        return;
    }

    for dir in cargo_target_runtime_dirs() {
        if fs::create_dir_all(&dir).is_ok() {
            let _ = fs::copy(&dll, dir.join("zyx.dll"));
        }
    }
}

fn cargo_target_runtime_dirs() -> Vec<PathBuf> {
    let out_dir = PathBuf::from(env::var_os("OUT_DIR").expect("OUT_DIR is set by Cargo"));
    let Some(profile_dir) = out_dir.ancestors().nth(3) else {
        return Vec::new();
    };
    vec![profile_dir.to_path_buf(), profile_dir.join("deps")]
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
