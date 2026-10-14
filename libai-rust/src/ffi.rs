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
pub type ExecutionFn = unsafe extern "C" fn(
    *mut c_void,
    *const c_char,
    *const c_char,
    *const c_char,
    *const c_char,
    *mut c_void,
);
pub type EventFn = unsafe extern "C" fn(*mut c_void, *const c_char);
pub type WorkflowNodeFn =
    unsafe extern "C" fn(*mut c_void, *const c_char, *const c_char, *mut c_void, *mut c_void);
pub type WorkflowReleaseFn = unsafe extern "C" fn(*mut c_void);
pub type McpTransportFn = unsafe extern "C" fn(
    *mut c_void,
    *const c_char,
    *const c_char,
    *const c_char,
    *mut c_void,
    *mut c_void,
);
pub type McpReleaseFn = unsafe extern "C" fn(*mut c_void);

#[repr(C)]
pub struct Hooks {
    pub user: *mut c_void,
    pub text: Option<TextFn>,
    pub authorize: Option<AuthorizeFn>,
    pub event: Option<EventFn>,
}

extern "C" {
    pub fn libai_workflow_create() -> *mut c_void;
    pub fn libai_workflow_add_node(
        graph: *mut c_void,
        descriptor: *const c_char,
        callback: WorkflowNodeFn,
        user: *mut c_void,
        release: WorkflowReleaseFn,
    ) -> c_int;
    pub fn libai_workflow_add_edge(
        graph: *mut c_void,
        from: *const c_char,
        to: *const c_char,
        label: *const c_char,
    ) -> c_int;
    pub fn libai_workflow_result_set(
        result: *mut c_void,
        wire: *const c_char,
        bytes: usize,
    ) -> c_int;
    pub fn libai_workflow_run(
        graph: *mut c_void,
        initial: *const c_char,
        options: *const c_char,
        store: Option<ExecutionFn>,
        user: *mut c_void,
        cancellation: *mut c_void,
    ) -> *mut c_char;
    pub fn libai_workflow_confirm_node(
        graph: *mut c_void,
        store: ExecutionFn,
        user: *mut c_void,
        scope: *const c_char,
        initial: *const c_char,
        revision: *const c_char,
        node: *const c_char,
        outcome: *const c_char,
        output: *const c_char,
        reference: *const c_char,
    ) -> c_int;
    pub fn libai_workflow_destroy(graph: *mut c_void) -> c_int;
    pub fn libai_invocation_remaining_ms(invocation: *mut c_void) -> i64;
    pub fn libai_invocation_retain_cancellation(invocation: *mut c_void) -> *mut c_void;
    pub fn libai_mcp_create(
        callback: McpTransportFn,
        user: *mut c_void,
        release: McpReleaseFn,
    ) -> *mut c_void;
    pub fn libai_mcp_result_response(
        result: *mut c_void,
        wire: *const c_char,
        bytes: usize,
    ) -> c_int;
    pub fn libai_mcp_result_error(result: *mut c_void, error: *const c_char);
    pub fn libai_mcp_stdio_available() -> c_int;
    pub fn libai_mcp_create_stdio(options: *const c_char) -> *mut c_void;
    pub fn libai_mcp_call(
        client: *mut c_void,
        operation: *const c_char,
        arguments: *const c_char,
        cancellation: *mut c_void,
    ) -> *mut c_char;
    pub fn libai_mcp_close(client: *mut c_void) -> c_int;
    pub fn libai_mcp_destroy(client: *mut c_void) -> c_int;
    pub fn libai_agent_add_mcp_tool(
        agent: *mut c_void,
        client: *mut c_void,
        remote: *const c_char,
        local: *const c_char,
        policy: *const c_char,
    ) -> c_int;
    pub fn libai_file_execution_store_available() -> c_int;
    pub fn libai_file_execution_store_create_plaintext(root: *const c_char) -> *mut c_void;
    pub fn libai_file_execution_store_open_values(
        store: *mut c_void,
        scope: *const c_char,
        revision: *mut *mut c_char,
        state: *mut *mut c_char,
    ) -> c_int;
    pub fn libai_file_execution_store_commit(
        store: *mut c_void,
        scope: *const c_char,
        revision: *const c_char,
        state: *const c_char,
    ) -> *mut c_char;
    pub fn libai_file_execution_store_close(store: *mut c_void, scope: *const c_char) -> c_int;
    pub fn libai_file_execution_store_destroy(store: *mut c_void) -> c_int;
    pub fn libai_execution_result_set(
        result: *mut c_void,
        revision: *const c_char,
        state: *const c_char,
        error: *const c_char,
    );
    pub fn libai_agent_set_execution_store(
        agent: *mut c_void,
        scope: *const c_char,
        callback: Option<ExecutionFn>,
        user: *mut c_void,
    ) -> c_int;
    pub fn libai_invocation_idempotency_key(invocation: *mut c_void) -> *const c_char;
    pub fn libai_execution_confirm(
        callback: ExecutionFn,
        user: *mut c_void,
        scope: *const c_char,
        input: *const c_char,
        instructions: *const c_char,
        revision: *const c_char,
        key: *const c_char,
        output: *const c_char,
        error: c_int,
        outcome: *const c_char,
        reference: *const c_char,
    ) -> c_int;
    pub fn libai_execution_adopt_deadline(
        callback: ExecutionFn,
        user: *mut c_void,
        scope: *const c_char,
        input: *const c_char,
        instructions: *const c_char,
        revision: *const c_char,
        remaining_ms: i64,
        reference: *const c_char,
    ) -> c_int;
    pub fn libai_agent_configure_output(
        agent: *mut c_void,
        schema: *const c_char,
        repairs: usize,
    ) -> c_int;
    pub fn libai_agent_configure_model(agent: *mut c_void, profile: *const c_char) -> c_int;
    pub fn libai_coding_available() -> c_int;
    pub fn libai_governance_call(
        operation: *const std::ffi::c_char,
        arguments: *const std::ffi::c_char,
    ) -> *mut std::ffi::c_char;
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
    pub fn libai_agent_add_tool_governed(
        agent: *mut c_void,
        name: *const c_char,
        description: *const c_char,
        schema: *const c_char,
        approval: c_int,
        revision: *const c_char,
        side_effect_free: c_int,
        tool: ToolFn,
        user: *mut c_void,
    ) -> c_int;
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

#[cfg(feature = "rag")]
pub type RagFn = unsafe extern "C" fn(
    *mut c_void,
    *const c_char,
    *const c_char,
    *const c_char,
    *mut c_void,
    *mut c_void,
);
#[cfg(feature = "rag")]
pub type RagReleaseFn = unsafe extern "C" fn(*mut c_void);
#[cfg(feature = "rag")]
extern "C" {
    pub fn libai_rag_create(
        kind: *const c_char,
        callback: RagFn,
        user: *mut c_void,
        release: RagReleaseFn,
        config: *const c_char,
    ) -> *mut c_void;
    pub fn libai_rag_create_splitter(config: *const c_char) -> *mut c_void;
    pub fn libai_rag_create_memory_store(config: *const c_char) -> *mut c_void;
    pub fn libai_rag_create_vector_retriever(
        embedding: *mut c_void,
        store: *mut c_void,
        config: *const c_char,
    ) -> *mut c_void;
    pub fn libai_rag_result_set(result: *mut c_void, wire: *const c_char, bytes: usize) -> c_int;
    pub fn libai_rag_result_error(result: *mut c_void, error: *const c_char);
    pub fn libai_rag_load(
        handle: *mut c_void,
        source: *const c_char,
        context: *const c_char,
        cancellation: *mut c_void,
    ) -> *mut c_char;
    pub fn libai_rag_prepare_index(
        splitter: *mut c_void,
        embedding: *mut c_void,
        docs: *const c_char,
        context: *const c_char,
        limits: *const c_char,
        cancellation: *mut c_void,
    ) -> *mut c_char;
    pub fn libai_rag_commit_index(
        store: *mut c_void,
        batch: *const c_char,
        context: *const c_char,
        cancellation: *mut c_void,
    ) -> c_int;
    pub fn libai_rag_retrieve(
        retriever: *mut c_void,
        request: *const c_char,
        context: *const c_char,
        cancellation: *mut c_void,
    ) -> *mut c_char;
    pub fn libai_rag_destroy(handle: *mut c_void) -> c_int;
    pub fn libai_agent_add_retrieval_tool(
        agent: *mut c_void,
        retriever: *mut c_void,
        name: *const c_char,
        binding: *const c_char,
    ) -> c_int;
}
