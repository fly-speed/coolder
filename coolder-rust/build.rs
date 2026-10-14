//! Cargo link arguments from libai-rust do not propagate to dependent executables.
fn main() {
    let target = std::env::var("TARGET").unwrap();
    if target.contains("apple") {
        println!("cargo:rustc-link-arg=-Wl,-rpath,@executable_path/../lib");
    } else if !target.contains("msvc") {
        println!("cargo:rustc-link-arg=-Wl,-rpath,$ORIGIN/../lib");
    }
    if let Ok(directory) = std::env::var("DEP_AI_C_LIBDIR") {
        if let Ok(manifest) = std::env::var("COOLDER_NATIVE_MANIFEST") {
            std::fs::write(&manifest, &directory).expect("write native runtime manifest");
            println!("cargo:rerun-if-changed={manifest}");
        }
        if !target.contains("msvc") {
            println!("cargo:rustc-link-arg=-Wl,-rpath,{directory}");
        }
    }
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-env-changed=COOLDER_NATIVE_MANIFEST");
}
