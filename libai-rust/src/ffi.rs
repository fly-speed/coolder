//! Exact C ABI declarations, isolated from the safe public facade.
use std::ffi::{c_char, c_int, c_void};

pub type ModelFn = unsafe extern "C" fn(*mut c_void, *const c_char, *mut c_void, *mut c_void);
pub type ToolFn = unsafe extern "C" fn(
    *mut c_void,
    *const c_char,
    *const c_char,
    *const c_char,
    *mut c_void,
    *mut c_void,
);
pub type TextFn = unsafe extern "C" fn(*mut c_void, *const c_char) -> c_int;
pub type AuthorizeFn =
    unsafe extern "C" fn(*mut c_void, *const c_char, *const c_char, c_int) -> c_int;
pub type EventFn = unsafe extern "C" fn(*mut c_void, *const c_char);

#[repr(C)]
pub struct Hooks {
    pub user: *mut c_void,
    pub text: Option<TextFn>,
    pub authorize: Option<AuthorizeFn>,
    pub event: Option<EventFn>,
}

extern "C" {
    pub fn libai_coding_available() -> c_int;
    pub fn libai_coding_call(
        root: *const c_char,
        operation: *const c_char,
        arguments: *const c_char,
    ) -> *mut c_char;
    pub fn libai_abi_version() -> u32;
    pub fn libai_last_error() -> *const c_char;
    pub fn libai_options_default(options: *mut crate::Options);
    pub fn libai_agent_create(
        model: ModelFn,
        user: *mut c_void,
        instructions: *const c_char,
    ) -> *mut c_void;
    pub fn libai_agent_create_provider(
        config: *const c_char,
        key: *const c_char,
        instructions: *const c_char,
    ) -> *mut c_void;
    pub fn libai_agent_messages(agent: *mut c_void) -> *mut c_char;
    pub fn libai_cancellation_create() -> *mut c_void;
    pub fn libai_cancellation_destroy(token: *mut c_void);
    pub fn libai_cancellation_cancel(token: *mut c_void);
    pub fn libai_cancellation_cancelled(token: *mut c_void) -> c_int;
    pub fn libai_agent_run_with_cancellation(
        agent: *mut c_void,
        input: *const c_char,
        options: *const crate::Options,
        hooks: *const Hooks,
        token: *mut c_void,
    ) -> *mut c_char;
    pub fn libai_agent_destroy(agent: *mut c_void) -> c_int;
    pub fn libai_agent_add_tool(
        agent: *mut c_void,
        name: *const c_char,
        description: *const c_char,
        schema: *const c_char,
        approval: c_int,
        tool: ToolFn,
        user: *mut c_void,
    ) -> c_int;
    pub fn libai_agent_run(
        agent: *mut c_void,
        input: *const c_char,
        options: *const crate::Options,
        hooks: *const Hooks,
    ) -> *mut c_char;
    pub fn libai_agent_cancel(agent: *mut c_void);
    pub fn libai_agent_reset(agent: *mut c_void) -> c_int;
    pub fn libai_string_free(text: *mut c_char);
    pub fn libai_response_text(output: *mut c_void, text: *const c_char, reasoning: *const c_char);
    pub fn libai_response_call(
        output: *mut c_void,
        id: *const c_char,
        name: *const c_char,
        arguments: *const c_char,
    );
    pub fn libai_response_usage(output: *mut c_void, input: usize, generated: usize);
    pub fn libai_response_error(output: *mut c_void, text: *const c_char, retryable: c_int);
    pub fn libai_tool_result_set(output: *mut c_void, text: *const c_char, error: c_int);
    pub fn libai_invocation_stopped(invocation: *mut c_void) -> c_int;
    pub fn libai_invocation_emit(invocation: *mut c_void, text: *const c_char) -> c_int;
}
