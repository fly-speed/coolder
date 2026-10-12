use libai::CodingService;
#[test]
fn coding_binding_availability_and_utf8() {
    let path = std::env::temp_dir().join(format!("libai-rust-coding-{}", std::process::id()));
    std::fs::create_dir_all(&path).unwrap();
    let service = CodingService::new(path.to_str().unwrap()).unwrap();
    if CodingService::available() {
        assert!(service
            .call("capabilities", "{}")
            .unwrap()
            .contains("workspace.read"));
        assert!(service
            .call(
                "workspace.create",
                r#"{"path":"rust-你好😀.txt","content":"Rust😀"}"#
            )
            .unwrap()
            .contains("true"));
        assert!(service
            .call("workspace.read", r#"{"path":"rust-你好😀.txt"}"#)
            .unwrap()
            .contains("Rust😀"));
        assert!(service
            .call("workspace.read", r#"{"path":"../escape"}"#)
            .unwrap()
            .contains("false"));
    } else {
        assert!(service
            .call("capabilities", "{}")
            .unwrap_err()
            .0
            .contains("LIBAI_CODING_FULL"));
    }
    std::fs::remove_dir_all(path).unwrap();
}
