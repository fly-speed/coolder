//! Safe owning facade over the portable C++ agent runtime.
//! Callbacks are synchronous; application JSON codecs can parse request/result strings.
mod callbacks;
mod ffi;
mod types;
use callbacks::{cstring, ModelHolder, ToolHolder};
use std::ffi::c_void;
pub use types::*;

/// Own a model and a tool registry. Mutable borrowing serializes session access.
/// The facade is intentionally neither Send nor Sync: callback state may be local.
pub struct Agent {
    pointer: *mut c_void,
    _model: Option<Box<ModelHolder>>,
    // C stores each holder address, so vector growth must not move the holders.
    #[allow(clippy::vec_box)]
    tools: Vec<Box<ToolHolder>>,
}
impl Agent {
    pub fn new(
        model: impl FnMut(&str, &Invocation<'_>) -> ModelResponse + 'static,
        instructions: &str,
    ) -> Result<Self, Error> {
        let instructions = cstring(instructions)?;
        let mut holder = Box::new(ModelHolder {
            callback: Box::new(model),
        });
        unsafe {
            if ffi::libai_abi_version() != 1 {
                return Err(Error("unsupported native ABI".into()));
            }
            let pointer = ffi::libai_agent_create(
                callbacks::model,
                (&mut *holder as *mut ModelHolder).cast(),
                instructions.as_ptr(),
            );
            if pointer.is_null() {
                return Err(native_error());
            }
            Ok(Self {
                pointer,
                _model: Some(holder),
                tools: Vec::new(),
            })
        }
    }
    /// Built-in desktop Provider with JSON endpoint options and an in-memory credential.
    pub fn from_provider(
        config_json: &str,
        api_key: &str,
        instructions: &str,
    ) -> Result<Self, Error> {
        let config = cstring(config_json)?;
        let key = cstring(api_key)?;
        let instructions = cstring(instructions)?;
        let pointer = unsafe {
            ffi::libai_agent_create_provider(config.as_ptr(), key.as_ptr(), instructions.as_ptr())
        };
        if pointer.is_null() {
            return Err(native_error());
        }
        Ok(Self {
            pointer,
            _model: None,
            tools: Vec::new(),
        })
    }
    /// Copy the full idle transcript as a JSON array.
    pub fn messages(&self) -> Result<String, Error> {
        owned_result(unsafe { ffi::libai_agent_messages(self.pointer) })
    }
    pub fn add_tool(
        &mut self,
        name: &str,
        description: &str,
        schema: &str,
        requires_approval: bool,
        tool: impl FnMut(ToolCall, &Invocation<'_>) -> ToolResult + 'static,
    ) -> Result<&mut Self, Error> {
        let name = cstring(name)?;
        let description = cstring(description)?;
        let schema = cstring(schema)?;
        let mut holder = Box::new(ToolHolder {
            callback: Box::new(tool),
        });
        let result = unsafe {
            ffi::libai_agent_add_tool(
                self.pointer,
                name.as_ptr(),
                description.as_ptr(),
                schema.as_ptr(),
                requires_approval as i32,
                callbacks::tool,
                (&mut *holder as *mut ToolHolder).cast(),
            )
        };
        if result != 0 {
            return Err(native_error());
        }
        // Box allocation is stable even when the ownership vector reallocates.
        self.tools.push(holder);
        Ok(self)
    }
    pub fn run(&mut self, input: &str) -> Result<String, Error> {
        self.run_with(input, &Options::default(), Hooks::default())
    }
    pub fn run_with(
        &mut self,
        input: &str,
        options: &Options,
        hooks: Hooks<'_>,
    ) -> Result<String, Error> {
        self.run_with_cancellation(input, options, hooks, None)
    }
    /// Reuse an independent stop token, including a token cancelled before this run.
    pub fn run_with_cancellation(
        &mut self,
        input: &str,
        options: &Options,
        hooks: Hooks<'_>,
        cancellation: Option<&Cancellation>,
    ) -> Result<String, Error> {
        let input = cstring(input)?;
        let mut holder = callbacks::HookHolder {
            hooks,
            agent: self.pointer,
        };
        let native_hooks = ffi::Hooks {
            user: (&mut holder as *mut callbacks::HookHolder<'_>).cast(),
            text: holder
                .hooks
                .on_text
                .as_ref()
                .map(|_| callbacks::stream as ffi::TextFn),
            authorize: holder
                .hooks
                .authorize
                .as_ref()
                .map(|_| callbacks::authorize as ffi::AuthorizeFn),
            event: holder
                .hooks
                .on_event
                .as_ref()
                .map(|_| callbacks::event as ffi::EventFn),
        };
        unsafe {
            let result = ffi::libai_agent_run_with_cancellation(
                self.pointer,
                input.as_ptr(),
                options,
                &native_hooks,
                cancellation.map_or(std::ptr::null_mut(), |token| token.pointer),
            );
            if result.is_null() {
                return Err(native_error());
            }
            let text = callbacks::text(result);
            ffi::libai_string_free(result);
            Ok(text)
        }
    }
    pub fn reset(&mut self) -> Result<(), Error> {
        if unsafe { ffi::libai_agent_reset(self.pointer) } == 0 {
            Ok(())
        } else {
            Err(native_error())
        }
    }
    pub fn cancel(&self) {
        unsafe {
            ffi::libai_agent_cancel(self.pointer);
        }
    }
}
impl Drop for Agent {
    fn drop(&mut self) {
        unsafe {
            ffi::libai_agent_destroy(self.pointer);
        }
    }
}
fn native_error() -> Error {
    Error(unsafe { callbacks::text(ffi::libai_last_error()) })
}

fn owned_result(pointer: *mut std::ffi::c_char) -> Result<String, Error> {
    if pointer.is_null() {
        return Err(native_error());
    }
    let text = unsafe { callbacks::text(pointer) };
    unsafe { ffi::libai_string_free(pointer) };
    Ok(text)
}

/// Cooperative stop state. Share with Arc for cancellation from another thread.
pub struct Cancellation {
    pointer: *mut c_void,
}
// Native token contains only shared ownership and atomic stop state.
unsafe impl Send for Cancellation {}
unsafe impl Sync for Cancellation {}
impl Cancellation {
    pub fn new() -> Result<Self, Error> {
        let pointer = unsafe { ffi::libai_cancellation_create() };
        if pointer.is_null() {
            return Err(native_error());
        }
        Ok(Self { pointer })
    }
    pub fn cancel(&self) {
        unsafe { ffi::libai_cancellation_cancel(self.pointer) }
    }
    pub fn cancelled(&self) -> bool {
        unsafe { ffi::libai_cancellation_cancelled(self.pointer) != 0 }
    }
}
impl Drop for Cancellation {
    fn drop(&mut self) {
        unsafe { ffi::libai_cancellation_destroy(self.pointer) }
    }
}

/// Root-confined coding facilities; the host owns authorization and JSON decoding.
pub struct CodingService {
    root: std::ffi::CString,
}
impl CodingService {
    /// Select a trusted existing directory; arguments cannot change this scope.
    pub fn new(root: &str) -> Result<Self, Error> {
        Ok(Self {
            root: cstring(root)?,
        })
    }
    /// Portable and mobile builds return false.
    pub fn available() -> bool {
        unsafe { ffi::libai_coding_available() != 0 }
    }
    /// Return {ok,...} JSON; operation failures use ok=false. ABI failures return Err.
    pub fn call(&self, operation: &str, arguments_json: &str) -> Result<String, Error> {
        let operation = cstring(operation)?;
        let arguments = cstring(arguments_json)?;
        unsafe {
            let result =
                ffi::libai_coding_call(self.root.as_ptr(), operation.as_ptr(), arguments.as_ptr());
            if result.is_null() {
                return Err(native_error());
            }
            let text = std::ffi::CStr::from_ptr(result)
                .to_string_lossy()
                .into_owned();
            ffi::libai_string_free(result);
            Ok(text)
        }
    }
}
