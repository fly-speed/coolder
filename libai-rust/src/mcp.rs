//! MCP protocol and recovery remain native; Rust owns stable callback state and borrowing.
use crate::{
    callbacks::{cstring, discard_panic, text},
    ffi, native_error, owned_result, Cancellation, Error, Invocation,
};
use std::{
    cell::RefCell,
    ffi::{c_char, c_void},
    marker::PhantomData,
    panic::{catch_unwind, AssertUnwindSafe},
    rc::Rc,
};

/// Host transport. Observe invocation.stopped() during blocking I/O.
/// request returns a complete JSON-RPC response; close must release reliably without panic.
pub trait McpTransport {
    fn request(
        &mut self,
        wire: &str,
        context_json: &str,
        invocation: &Invocation<'_>,
    ) -> Result<String, Error>;
    fn notify(
        &mut self,
        wire: &str,
        context_json: &str,
        invocation: &Invocation<'_>,
    ) -> Result<(), Error>;
    fn close(&mut self);
}
pub type SharedMcpTransport = Rc<RefCell<dyn McpTransport>>;
struct Holder(SharedMcpTransport);

unsafe extern "C" fn callback(
    user: *mut c_void,
    operation: *const c_char,
    wire: *const c_char,
    context: *const c_char,
    result: *mut c_void,
    pointer: *mut c_void,
) {
    let response = catch_unwind(AssertUnwindSafe(|| -> Result<(), Error> {
        let holder = &*(user as *const Holder);
        let mut transport = holder
            .0
            .try_borrow_mut()
            .map_err(|_| Error("MCP transport is busy".into()))?;
        let op = text(operation);
        if op == "close" {
            transport.close();
            return Ok(());
        }
        if pointer.is_null() {
            return Err(Error("MCP invocation missing".into()));
        }
        let invocation = Invocation {
            pointer,
            lifetime: PhantomData,
        };
        match op.as_str() {
            "request" => {
                let response = transport.request(&text(wire), &text(context), &invocation)?;
                let response = cstring(&response)?;
                if ffi::libai_mcp_result_response(
                    result,
                    response.as_ptr(),
                    response.as_bytes().len(),
                ) != 0
                {
                    return Err(Error("MCP response exceeds native bounds".into()));
                }
            }
            "notify" => transport.notify(&text(wire), &text(context), &invocation)?,
            _ => return Err(Error("unknown MCP callback operation".into())),
        }
        Ok(())
    }));
    match response {
        Ok(Ok(())) => (),
        Ok(Err(error)) => {
            let mut message = error.0.replace('\0', "");
            if message.len() > 4096 {
                let mut end = 4096;
                while !message.is_char_boundary(end) {
                    end -= 1;
                }
                message.truncate(end);
            }
            let encoded = cstring(&message)
                .unwrap_or_else(|_| std::ffi::CString::new("MCP callback failed").unwrap());
            ffi::libai_mcp_result_error(result, encoded.as_ptr());
        }
        Err(payload) => {
            discard_panic(payload);
            ffi::libai_mcp_result_error(result, c"Rust MCP transport panicked".as_ptr());
        }
    }
}
unsafe extern "C" fn release(user: *mut c_void) {
    // A user transport's destructor must not unwind across the C boundary.
    if let Err(payload) = catch_unwind(AssertUnwindSafe(|| {
        drop(Box::from_raw(user as *mut Holder))
    })) {
        discard_panic(payload);
    }
}
pub(crate) fn quote(value: &str) -> String {
    let mut result = String::from("\"");
    for ch in value.chars() {
        match ch {
            '"' => result.push_str("\\\""),
            '\\' => result.push_str("\\\\"),
            c if c < ' ' => result.push_str(&format!("\\u{:04x}", c as u32)),
            c => result.push(c),
        }
    }
    result.push('"');
    result
}

pub struct McpOptions<'a> {
    pub timeout_ms: u64,
    /// Forwarded only by tools/call. It does not prove server-side deduplication.
    pub idempotency_key: &'a str,
    pub cancellation: Option<&'a Cancellation>,
}
impl Default for McpOptions<'_> {
    fn default() -> Self {
        Self {
            timeout_ms: 30000,
            idempotency_key: "",
            cancellation: None,
        }
    }
}
pub struct McpBinding<'a> {
    pub parameters_schema: &'a str,
    pub output_schema: &'a str,
    pub execution_revision: &'a str,
    pub requires_approval: bool,
    pub side_effect_free: bool,
    pub timeout_ms: u64,
}
impl Default for McpBinding<'_> {
    fn default() -> Self {
        Self {
            parameters_schema: "",
            output_schema: "",
            execution_revision: "",
            requires_approval: true,
            side_effect_free: false,
            timeout_ms: 30000,
        }
    }
}
impl McpBinding<'_> {
    pub(crate) fn json(&self) -> String {
        format!("{{\"parameters_schema\":{},\"output_schema\":{},\"execution_revision\":{},\"requires_approval\":{},\"side_effect_free\":{},\"timeout_ms\":{}}}",
            quote(self.parameters_schema),quote(self.output_schema),quote(self.execution_revision),self.requires_approval,self.side_effect_free,self.timeout_ms)
    }
}
/// Same-thread owner. Agent bindings retain the underlying connection after Drop.
/// No Send/Sync promise is made for host callback state.
pub struct McpClient {
    pub(crate) pointer: *mut c_void,
    _local: PhantomData<Rc<()>>,
}
impl McpClient {
    pub fn new(transport: SharedMcpTransport) -> Result<Self, Error> {
        let user = Box::into_raw(Box::new(Holder(transport)));
        let pointer = unsafe { ffi::libai_mcp_create(callback, user.cast(), release) };
        if pointer.is_null() {
            unsafe {
                drop(Box::from_raw(user));
            }
            return Err(native_error());
        }
        Ok(Self {
            pointer,
            _local: PhantomData,
        })
    }
    /// Complete JSON options: executable, arguments, inherit_environment, environment.
    pub fn stdio(options_json: &str) -> Result<Self, Error> {
        let options = cstring(options_json)?;
        let pointer = unsafe { ffi::libai_mcp_create_stdio(options.as_ptr()) };
        if pointer.is_null() {
            return Err(native_error());
        }
        Ok(Self {
            pointer,
            _local: PhantomData,
        })
    }
    pub fn stdio_available() -> bool {
        unsafe { ffi::libai_mcp_stdio_available() != 0 }
    }
    /// Explicit host operation; direct call_tool does not acquire Agent governance/log guarantees.
    pub fn call(
        &mut self,
        operation: &str,
        arguments_json: &str,
        cancellation: Option<&Cancellation>,
    ) -> Result<String, Error> {
        let operation = cstring(operation)?;
        let arguments = cstring(arguments_json)?;
        owned_result(unsafe {
            ffi::libai_mcp_call(
                self.pointer,
                operation.as_ptr(),
                arguments.as_ptr(),
                cancellation.map_or(std::ptr::null_mut(), |c| c.pointer),
            )
        })
    }
    pub fn initialize(
        &mut self,
        name: &str,
        version: &str,
        options: &McpOptions<'_>,
    ) -> Result<String, Error> {
        self.call(
            "initialize",
            &format!(
                "{{\"name\":{},\"version\":{},\"timeout_ms\":{}}}",
                quote(name),
                quote(version),
                options.timeout_ms
            ),
            options.cancellation,
        )
    }
    pub fn list_tools(&mut self, options: &McpOptions<'_>) -> Result<String, Error> {
        self.call(
            "list_tools",
            &format!("{{\"timeout_ms\":{}}}", options.timeout_ms),
            options.cancellation,
        )
    }
    pub fn tool(&mut self, name: &str) -> Result<String, Error> {
        self.call("tool", &format!("{{\"name\":{}}}", quote(name)), None)
    }
    pub fn call_tool(
        &mut self,
        name: &str,
        arguments_json: &str,
        options: &McpOptions<'_>,
    ) -> Result<String, Error> {
        self.call(
            "call_tool",
            &format!(
                "{{\"name\":{},\"arguments_json\":{},\"timeout_ms\":{},\"idempotency_key\":{}}}",
                quote(name),
                quote(arguments_json),
                options.timeout_ms,
                quote(options.idempotency_key)
            ),
            options.cancellation,
        )
    }
    /// Close every binding sharing this connection. Repeat calls are harmless.
    pub fn close(&mut self) -> Result<(), Error> {
        if unsafe { ffi::libai_mcp_close(self.pointer) } != 0 {
            return Err(native_error());
        }
        Ok(())
    }
}
impl Drop for McpClient {
    fn drop(&mut self) {
        unsafe {
            ffi::libai_mcp_destroy(self.pointer);
        }
    }
}
