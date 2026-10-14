//! Catch every Rust panic before it can unwind through C/C++ frames.
use crate::{ffi, Error, Hooks, Invocation, ModelResponse, ToolCall, ToolResult};
use std::{
    ffi::{c_char, c_int, c_void, CStr, CString},
    marker::PhantomData,
    panic::{catch_unwind, AssertUnwindSafe},
};

pub(crate) fn discard_panic(payload: Box<dyn std::any::Any + Send>) {
    // A panic payload may itself have a panicking destructor. Keep that second
    // unwind inside Rust too; only such faulty nested payloads are abandoned.
    if let Err(nested) = catch_unwind(AssertUnwindSafe(|| drop(payload))) {
        std::mem::forget(nested);
    }
}

pub type Model = dyn FnMut(&str, &Invocation<'_>) -> ModelResponse;
pub type Tool = dyn FnMut(ToolCall, &Invocation<'_>) -> ToolResult;
pub struct ModelHolder {
    pub callback: Box<Model>,
}
pub struct ToolHolder {
    pub callback: Box<Tool>,
}
pub struct HookHolder<'a> {
    pub hooks: Hooks<'a>,
    pub agent: *mut c_void,
}

pub fn cstring(text: &str) -> Result<CString, Error> {
    CString::new(text).map_err(|_| Error("text contains NUL".into()))
}
pub unsafe fn text(pointer: *const c_char) -> String {
    CStr::from_ptr(pointer).to_string_lossy().into_owned()
}
fn output(response: ModelResponse, pointer: *mut c_void) -> Result<(), Error> {
    let visible = cstring(&response.text)?;
    let reasoning = cstring(&response.reasoning)?;
    let error = cstring(&response.error)?;
    unsafe {
        ffi::libai_response_text(pointer, visible.as_ptr(), reasoning.as_ptr());
        ffi::libai_response_usage(pointer, response.input_tokens, response.output_tokens);
        ffi::libai_response_error(pointer, error.as_ptr(), response.retryable as c_int);
    }
    for call in response.calls {
        let id = cstring(&call.id)?;
        let name = cstring(&call.name)?;
        let args = cstring(&call.arguments)?;
        unsafe {
            ffi::libai_response_call(pointer, id.as_ptr(), name.as_ptr(), args.as_ptr());
        }
    }
    Ok(())
}

pub unsafe extern "C" fn model(
    user: *mut c_void,
    request: *const c_char,
    builder: *mut c_void,
    boundary: *mut c_void,
) {
    let result = catch_unwind(AssertUnwindSafe(|| {
        let holder = &mut *(user as *mut ModelHolder);
        let invocation = Invocation {
            pointer: boundary,
            lifetime: PhantomData,
        };
        output((holder.callback)(&text(request), &invocation), builder)
    }));
    match result {
        Ok(Ok(())) => (),
        Ok(Err(error)) => {
            let message = cstring(&error.0).unwrap();
            ffi::libai_response_error(builder, message.as_ptr(), 0);
        }
        Err(payload) => {
            discard_panic(payload);
            ffi::libai_response_error(builder, c"Rust model callback panicked".as_ptr(), 0);
        }
    }
}
pub unsafe extern "C" fn tool(
    user: *mut c_void,
    id: *const c_char,
    name: *const c_char,
    arguments: *const c_char,
    builder: *mut c_void,
    boundary: *mut c_void,
) {
    let result = catch_unwind(AssertUnwindSafe(|| {
        let holder = &mut *(user as *mut ToolHolder);
        let invocation = Invocation {
            pointer: boundary,
            lifetime: PhantomData,
        };
        let call = ToolCall {
            id: text(id),
            name: text(name),
            arguments: text(arguments),
        };
        let output = (holder.callback)(call, &invocation);
        let content = cstring(&output.text)?;
        ffi::libai_tool_result_set(builder, content.as_ptr(), output.error as c_int);
        Ok::<(), Error>(())
    }));
    match result {
        Ok(Ok(())) => (),
        other => {
            if let Err(payload) = other {
                discard_panic(payload);
            }
            ffi::libai_tool_result_set(builder, c"Rust tool callback failed".as_ptr(), 1);
        }
    }
}
pub unsafe extern "C" fn stream(user: *mut c_void, delta: *const c_char) -> c_int {
    let holder = &mut *(user as *mut HookHolder<'_>);
    let result = catch_unwind(AssertUnwindSafe(|| {
        (holder.hooks.on_text.as_mut().unwrap())(&text(delta))
    }));
    match result {
        Ok(value) => value as c_int,
        Err(payload) => {
            discard_panic(payload);
            ffi::libai_agent_cancel(holder.agent);
            0
        }
    }
}
pub unsafe extern "C" fn authorize(
    user: *mut c_void,
    name: *const c_char,
    args: *const c_char,
    required: c_int,
) -> c_int {
    let holder = &mut *(user as *mut HookHolder<'_>);
    let result = catch_unwind(AssertUnwindSafe(|| {
        (holder.hooks.authorize.as_mut().unwrap())(&text(name), &text(args), required != 0)
    }));
    match result {
        Ok(value) => value as c_int,
        Err(payload) => {
            discard_panic(payload);
            ffi::libai_agent_cancel(holder.agent);
            0
        }
    }
}
pub unsafe extern "C" fn event(user: *mut c_void, value: *const c_char) {
    let holder = &mut *(user as *mut HookHolder<'_>);
    if let Err(payload) = catch_unwind(AssertUnwindSafe(|| {
        (holder.hooks.on_event.as_mut().unwrap())(&text(value))
    })) {
        discard_panic(payload);
        ffi::libai_agent_cancel(holder.agent);
    }
}
