use std::{
    env,
    path::{Path, PathBuf},
};

#[cfg(feature = "bundled")]
use std::{fs, process::Command};

fn main() {
    println!("cargo:rerun-if-env-changed=ZYX_LIB_DIR");
    println!("cargo:rerun-if-env-changed=ZYXDB_SYS_USE_CONAN");
    println!("cargo:rerun-if-env-changed=CMAKE_TOOLCHAIN_FILE");
    println!("cargo:rerun-if-env-changed=CMAKE_GENERATOR");
    println!("cargo:rerun-if-env-changed=CMAKE_BUILD_TYPE");

    let bundled = env::var_os("CARGO_FEATURE_BUNDLED").is_some();
    let system = env::var_os("CARGO_FEATURE_SYSTEM").is_some();

    match (bundled, system) {
        (true, false) => build_bundled(),
        (false, true) => link_system(),
        (true, true) => panic!("features `bundled` and `system` are mutually exclusive"),
        (false, false) => panic!("enable either the `bundled` or `system` feature"),
    }
}

#[cfg(feature = "bundled")]
fn build_bundled() {
    let source_dir = zyx_src::source_dir();
    emit_source_reruns(&source_dir);

    let out_dir = PathBuf::from(env::var_os("OUT_DIR").expect("OUT_DIR is set by Cargo"));
    let generator = cmake_generator();
    let build_dir = out_dir.join(if generator.is_some() {
        "zyx-build-ninja"
    } else {
        "zyx-build"
    });
    let install_dir = out_dir.join("zyx-install");
    let profile = cmake_profile();

    fs::create_dir_all(&build_dir).expect("failed to create bundled ZYX build directory");
    fs::create_dir_all(&install_dir).expect("failed to create bundled ZYX install directory");

    let mut configure = Command::new("cmake");
    if let Some(generator) = generator {
        configure.arg("-G").arg(generator);
    }
    configure
        .arg("-S")
        .arg(&source_dir)
        .arg("-B")
        .arg(&build_dir)
        .arg(format!("-DCMAKE_BUILD_TYPE={profile}"))
        .arg(format!("-DCMAKE_INSTALL_PREFIX={}", install_dir.display()))
        .arg("-DZYX_BUILD_TESTS=OFF")
        .arg("-DZYX_BUILD_APPS=OFF")
        .arg("-DZYX_BUILD_PYTHON=OFF")
        .arg("-DZYX_BUILD_SHARED_LIBRARY=ON")
        .arg("-DZYX_BUILD_STATIC_LIBRARY=OFF")
        .arg("-DZYX_INSTALL_PKGCONFIG=OFF")
        .arg("-DBUILD_SHARED_LIBS=ON")
        .arg("-DCMAKE_POSITION_INDEPENDENT_CODE=ON");

    if let Some(toolchain) = bundled_toolchain_file(&source_dir, &build_dir, profile) {
        configure.arg(format!("-DCMAKE_TOOLCHAIN_FILE={}", toolchain.display()));
    }

    run(&mut configure, "configure bundled ZYX native library");

    let mut build = Command::new("cmake");
    build
        .arg("--build")
        .arg(&build_dir)
        .arg("--target")
        .arg("install")
        .arg("--config")
        .arg(profile);
    run(&mut build, "build bundled ZYX native library");

    link_bundled_install(&install_dir);
}

#[cfg(feature = "bundled")]
#[cfg(feature = "bundled")]
fn cmake_generator() -> Option<&'static str> {
    if env::var_os("CMAKE_GENERATOR").is_some() {
        return None;
    }

    Command::new("ninja")
        .arg("--version")
        .output()
        .ok()
        .filter(|output| output.status.success())
        .map(|_| "Ninja")
}

#[cfg(feature = "bundled")]
fn bundled_toolchain_file(source_dir: &Path, build_dir: &Path, profile: &str) -> Option<PathBuf> {
    if let Some(path) = env::var_os("CMAKE_TOOLCHAIN_FILE") {
        return Some(PathBuf::from(path));
    }

    if env_flag("ZYXDB_SYS_USE_CONAN") {
        let conan_dir = build_dir.join("conan");
        fs::create_dir_all(&conan_dir).expect("failed to create Conan output directory");

        let mut install = Command::new("conan");
        install
            .arg("install")
            .arg(source_dir)
            .arg(format!("--output-folder={}", conan_dir.display()))
            .arg("--build=missing")
            .arg("--build=antlr4-cppruntime/*")
            .arg("-s")
            .arg(format!("build_type={profile}"))
            .arg("-s")
            .arg("compiler.cppstd=20")
            .arg("-o")
            .arg("with_tests=False");
        run(&mut install, "install bundled ZYX dependencies with Conan");

        return Some(conan_dir.join("conan_toolchain.cmake"));
    }

    None
}

#[cfg(feature = "bundled")]
fn link_bundled_install(install_dir: &Path) {
    let lib_dir = install_dir.join("lib");
    let lib64_dir = install_dir.join("lib64");
    let bin_dir = install_dir.join("bin");

    for dir in [&lib_dir, &lib64_dir, &bin_dir] {
        if dir.exists() {
            println!("cargo:rustc-link-search=native={}", dir.display());
        }
    }
    if lib_dir.exists() {
        println!("cargo:LIB_DIR={}", lib_dir.display());
    } else if lib64_dir.exists() {
        println!("cargo:LIB_DIR={}", lib64_dir.display());
    }
    if bin_dir.exists() {
        println!("cargo:BIN_DIR={}", bin_dir.display());
    }

    if cfg!(target_os = "macos") || cfg!(target_os = "linux") {
        for dir in [&lib_dir, &lib64_dir] {
            if dir.exists() {
                println!("cargo:rustc-link-arg=-Wl,-rpath,{}", dir.display());
            }
        }
    }

    if cfg!(target_os = "windows") {
        copy_windows_runtime_dll(&bin_dir);
    }

    println!("cargo:rustc-link-lib=dylib=zyx");
}

#[cfg(feature = "bundled")]
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

#[cfg(feature = "bundled")]
fn cargo_target_runtime_dirs() -> Vec<PathBuf> {
    let out_dir = PathBuf::from(env::var_os("OUT_DIR").expect("OUT_DIR is set by Cargo"));
    let Some(profile_dir) = out_dir.ancestors().nth(3) else {
        return Vec::new();
    };
    vec![profile_dir.to_path_buf(), profile_dir.join("deps")]
}

fn link_system() {
    let lib_dir = env::var_os("ZYX_LIB_DIR")
        .map(PathBuf::from)
        .or_else(discover_repo_lib_dir);
    if let Some(lib_dir) = lib_dir {
        println!("cargo:rustc-link-search=native={}", lib_dir.display());
        emit_runtime_path(&lib_dir);
    }

    println!("cargo:rustc-link-lib=dylib=zyx");
}

#[cfg(feature = "bundled")]
fn cmake_profile() -> &'static str {
    env::var("CMAKE_BUILD_TYPE")
        .ok()
        .filter(|value| !value.is_empty())
        .map(|value| {
            if value.eq_ignore_ascii_case("debug") {
                "Debug"
            } else {
                "Release"
            }
        })
        .unwrap_or_else(|| match env::var("PROFILE").as_deref() {
            Ok("debug") => "Debug",
            _ => "Release",
        })
}

#[cfg(feature = "bundled")]
fn env_flag(name: &str) -> bool {
    env::var(name)
        .map(|value| {
            matches!(
                value.as_str(),
                "1" | "true" | "TRUE" | "on" | "ON" | "yes" | "YES"
            )
        })
        .unwrap_or(false)
}

#[cfg(feature = "bundled")]
fn emit_source_reruns(source_dir: &Path) {
    for relative in [
        "CMakeLists.txt",
        "cmake",
        "include",
        "src",
        "lib/inputxx",
        "conanfile.py",
    ] {
        let path = source_dir.join(relative);
        if path.is_dir() {
            emit_dir_reruns(&path);
        } else if path.exists() {
            println!("cargo:rerun-if-changed={}", path.display());
        }
    }
}

#[cfg(feature = "bundled")]
fn emit_dir_reruns(dir: &Path) {
    let Ok(entries) = fs::read_dir(dir) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            emit_dir_reruns(&path);
        } else {
            println!("cargo:rerun-if-changed={}", path.display());
        }
    }
}

fn emit_runtime_path(lib_dir: &Path) {
    if cfg!(target_os = "macos") || cfg!(target_os = "linux") {
        println!("cargo:rustc-link-arg=-Wl,-rpath,{}", lib_dir.display());
    }
}

#[cfg(not(feature = "bundled"))]
fn build_bundled() {
    panic!("the `bundled` feature is not enabled");
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

#[cfg(feature = "bundled")]
fn run(command: &mut Command, action: &str) {
    let status = command
        .status()
        .unwrap_or_else(|error| panic!("failed to {action}: {error}"));
    if !status.success() {
        panic!("failed to {action}: command exited with {status}");
    }
}
