//! Public owned values and a callback-scoped execution boundary.
use crate::ffi;
use std::{
    ffi::{c_void, CString},
    fmt,
    marker::PhantomData,
};

#[derive(Debug)]
pub struct Error(pub String);
impl fmt::Display for Error {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.0)
    }
}
impl std::error::Error for Error {}

#[derive(Default, Debug)]
pub struct ToolCall {
    pub id: String,
    pub name: String,
    pub arguments: String,
}
#[derive(Default, Debug)]
pub struct ModelResponse {
    pub text: String,
    pub reasoning: String,
    pub calls: Vec<ToolCall>,
    pub input_tokens: usize,
    pub output_tokens: usize,
    pub error: String,
    pub retryable: bool,
}
#[derive(Default, Debug)]
pub struct ToolResult {
    pub text: String,
    pub error: bool,
}

/// ABI-compatible budgets; defaults are initialized by the native SDK.
#[repr(C)]
#[derive(Clone, Debug)]
pub struct Options {
    pub max_model_calls: usize,
    pub max_tool_calls: usize,
    pub max_output_tokens: usize,
    pub max_tokens_per_call: usize,
    pub max_context_bytes: usize,
    pub max_tool_result_bytes: usize,
    pub max_model_retries: usize,
    pub timeout_ms: i64,
}
impl Default for Options {
    fn default() -> Self {
        let mut value = std::mem::MaybeUninit::uninit();
        unsafe {
            ffi::libai_options_default(value.as_mut_ptr());
            value.assume_init()
        }
    }
}

/// Borrowed native callback state cannot outlive the callback that supplied it.
pub struct Invocation<'a> {
    pub(crate) pointer: *mut c_void,
    pub(crate) lifetime: PhantomData<&'a mut ()>,
}
impl Invocation<'_> {
    /// Parent deadline: -1 unlimited, 0 expired. Never turn 0 into an unlimited child.
    pub fn remaining_ms(&self) -> i64 {
        unsafe { ffi::libai_invocation_remaining_ms(self.pointer) }
    }
    /// Own a copy of the shared stop state. Fail if absent or allocation fails.
    pub fn retain_cancellation(&self) -> Result<crate::Cancellation, Error> {
        let pointer = unsafe { ffi::libai_invocation_retain_cancellation(self.pointer) };
        if pointer.is_null() {
            return Err(Error(
                "invocation has no cancellation token or retain failed".into(),
            ));
        }
        Ok(crate::Cancellation { pointer })
    }
    /// Copy the stable durable effect key while inside a callback; empty if disabled.
    pub fn idempotency_key(&self) -> String {
        unsafe { crate::callbacks::text(ffi::libai_invocation_idempotency_key(self.pointer)) }
    }
    pub fn stopped(&self) -> bool {
        unsafe { ffi::libai_invocation_stopped(self.pointer) != 0 }
    }
    pub fn emit(&self, text: &str) -> Result<bool, Error> {
        let text = CString::new(text).map_err(|_| Error("text contains NUL".into()))?;
        Ok(unsafe { ffi::libai_invocation_emit(self.pointer, text.as_ptr()) != 0 })
    }
}

pub type TextSink<'a> = &'a mut dyn FnMut(&str) -> bool;
pub type Authorization<'a> = &'a mut dyn FnMut(&str, &str, bool) -> bool;
pub type EventSink<'a> = &'a mut dyn FnMut(&str);
/// Hooks are borrowed only for the duration of a synchronous run.
#[derive(Default)]
pub struct Hooks<'a> {
    pub on_text: Option<TextSink<'a>>,
    pub authorize: Option<Authorization<'a>>,
    pub on_event: Option<EventSink<'a>>,
}
