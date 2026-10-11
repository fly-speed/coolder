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
    _model: Box<ModelHolder>,
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
                _model: holder,
                tools: Vec::new(),
            })
        }
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
            let result = ffi::libai_agent_run(self.pointer, input.as_ptr(), options, &native_hooks);
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
