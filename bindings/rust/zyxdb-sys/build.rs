use std::env;

fn main() {
    println!("cargo:rerun-if-env-changed=ZYX_LIB_DIR");

    if let Ok(lib_dir) = env::var("ZYX_LIB_DIR") {
        println!("cargo:rustc-link-search=native={lib_dir}");
    }

    println!("cargo:rustc-link-lib=dylib=zyx");
}
