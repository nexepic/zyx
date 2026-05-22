use std::{
    env,
    path::{Path, PathBuf},
};

fn main() {
    println!("cargo:rerun-if-env-changed=ZYX_LIB_DIR");

    if let Ok(lib_dir) = env::var("ZYX_LIB_DIR") {
        println!("cargo:rustc-link-search=native={lib_dir}");
    } else if let Some(lib_dir) = discover_repo_lib_dir() {
        println!("cargo:rustc-link-search=native={}", lib_dir.display());
    }

    println!("cargo:rustc-link-lib=dylib=zyx");
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
