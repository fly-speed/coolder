//! Cargo link arguments from libai-rust do not propagate to dependent executables.
fn main() {
    let target = std::env::var("TARGET").unwrap();
    if target.contains("apple") {
        println!("cargo:rustc-link-arg=-Wl,-rpath,@executable_path/../lib");
    } else if !target.contains("msvc") {
        println!("cargo:rustc-link-arg=-Wl,-rpath,$ORIGIN/../lib");
    }
    if let Ok(directory) = std::env::var("DEP_AI_C_LIBDIR") {
        if !target.contains("msvc") {
            println!("cargo:rustc-link-arg=-Wl,-rpath,{directory}");
        }
    }
    println!("cargo:rerun-if-changed=build.rs");
}
