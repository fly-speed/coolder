//! Build the portable C ABI locally; the optional RAG codec uses cached Cargo dependencies.
use std::{env, path::PathBuf, process::Command};

fn execute(command: &mut Command) {
    assert!(
        command
            .status()
            .expect("failed to execute native build")
            .success(),
        "native build failed"
    );
}

fn main() {
    let root = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let destination = PathBuf::from(env::var("OUT_DIR").unwrap());
    let install = destination.join("install");
    let target = env::var("TARGET").unwrap();
    let host = env::var("HOST").unwrap();
    let cmake = env::var("CMAKE").unwrap_or_else(|_| "cmake".into());
    let full = env::var("LIBAI_CODING_FULL")
        .map(|v| v == "ON" || v == "1")
        .unwrap_or(false);
    let mut configure = Command::new(&cmake);
    configure
        .arg("-S")
        .arg(root.join("../libai-c"))
        .arg("-B")
        .arg(destination.join("native"))
        .arg(if full {
            "-DBUILD_SHARED_LIBS=ON"
        } else {
            "-DBUILD_SHARED_LIBS=OFF"
        })
        .arg(if full {
            "-DLIBAI_CODING_FULL=ON"
        } else {
            "-DLIBAI_CODING_FULL=OFF"
        })
        .arg("-DLIBAI_C_TESTS=OFF")
        .arg("-DCMAKE_BUILD_TYPE=Release")
        .arg("-DCMAKE_INSTALL_LIBDIR=lib")
        .arg(format!("-DCMAKE_INSTALL_PREFIX={}", install.display()));
    for name in [
        "ACL_PATH",
        "OPENSSL_ROOT_DIR",
        "OPENSSL_PATH",
    ] {
        if let Ok(value) = env::var(name) {
            configure.arg(format!("-D{name}={value}"));
        }
        println!("cargo:rerun-if-env-changed={name}");
    }
    if let Ok(toolchain) = env::var("LIBAI_CMAKE_TOOLCHAIN_FILE") {
        configure.arg(format!("-DCMAKE_TOOLCHAIN_FILE={toolchain}"));
    } else if target != host {
        panic!(
            "cross builds require LIBAI_CMAKE_TOOLCHAIN_FILE; see Android/iOS build documentation"
        );
    }
    execute(&mut configure);
    execute(
        Command::new(&cmake)
            .arg("--build")
            .arg(destination.join("native"))
            .arg("--config")
            .arg("Release"),
    );
    execute(
        Command::new(&cmake)
            .arg("--install")
            .arg(destination.join("native"))
            .arg("--config")
            .arg("Release"),
    );
    println!(
        "cargo:rustc-link-search=native={}",
        install.join("lib").display()
    );
    println!("cargo:libdir={}", install.join("lib").display());
    if full {
        println!("cargo:rustc-link-lib=dylib=ai_c");
        if !target.contains("msvc") {
            println!(
                "cargo:rustc-link-arg=-Wl,-rpath,{}",
                install.join("lib").display()
            );
        }
    } else {
        println!("cargo:rustc-link-lib=static=ai_c");
    }
    if target.contains("apple") {
        println!("cargo:rustc-link-lib=c++");
    } else if !target.contains("msvc") {
        println!("cargo:rustc-link-lib=stdc++");
        println!("cargo:rustc-link-lib=pthread");
    }
    println!("cargo:rerun-if-changed=../libai-c");

    println!("cargo:rerun-if-env-changed=LIBAI_CMAKE_TOOLCHAIN_FILE");
    println!("cargo:rerun-if-env-changed=CMAKE");
    println!("cargo:rerun-if-env-changed=LIBAI_CODING_FULL");
    println!("cargo:rerun-if-changed=../libai");
    println!("cargo:rerun-if-changed=../libai-c++/cmake");
}
