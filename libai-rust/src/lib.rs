//! Safe owning facade over the portable C++ agent runtime.
//! Callbacks are synchronous; application JSON codecs can parse request/result strings.
mod callbacks;
mod execution;
mod file_execution;
pub use file_execution::FileExecutionStore;
mod ffi;
mod mcp;
mod workflow;
mod task_contract;
mod task_values;
pub use task_contract::{TaskContract,TaskEvidence,TaskRequirements,TaskChecks,TaskEvaluation};
pub use execution::{
    adopt_execution_deadline, DeadlineAdoption,
    confirm_execution_effect, ExecutionReceipt, ExecutionSnapshot, ExecutionStore,
    SharedExecutionStore,
};
pub use mcp::{McpBinding, McpClient, McpOptions, McpTransport, SharedMcpTransport};
pub use workflow::{
    Workflow, WorkflowContext, WorkflowNodeResult, WorkflowNodeSpec, WorkflowOptions,
    WorkflowReceipt,
};
#[cfg(feature = "rag")]
mod rag;
#[cfg(feature = "rag")]
mod rag_types;
#[cfg(feature = "rag")]
mod rag_values;
#[cfg(feature = "rag")]
pub use rag::{RagEmbedding, RagLoader, RagRetriever, RagSplitter, RagVectorStore};
#[cfg(feature = "rag")]
pub use rag_types::*;
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
    execution: Option<Box<execution::StoreHolder>>,
}
impl Agent {
    /// Bind trusted RAG policy; model can supply query/top_k only.
    #[cfg(feature = "rag")]
    pub fn add_retrieval_tool(
        &mut self,
        retriever: &RagRetriever,
        name: &str,
        binding: &RagBinding,
    ) -> Result<&mut Self, Error> {
        let retriever = retriever.pointer()?;
        let name = cstring(name)?;
        let policy = cstring(&binding.json()?)?;
        if unsafe {
            ffi::libai_agent_add_retrieval_tool(
                self.pointer,
                retriever,
                name.as_ptr(),
                policy.as_ptr(),
            )
        } != 0
        {
            return Err(native_error());
        }
        Ok(self)
    }
    /// Bind discovered tools with trusted local Schema/revision; remote hints grant no authority.
    pub fn add_mcp_tool(
        &mut self,
        client: &McpClient,
        remote: &str,
        local: &str,
        binding: &McpBinding<'_>,
    ) -> Result<&mut Self, Error> {
        let remote = cstring(remote)?;
        let local = cstring(local)?;
        let policy = cstring(&binding.json())?;
        if unsafe {
            ffi::libai_agent_add_mcp_tool(
                self.pointer,
                client.pointer,
                remote.as_ptr(),
                local.as_ptr(),
                policy.as_ptr(),
            )
        } != 0
        {
            return Err(native_error());
        }
        Ok(self)
    }
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
                execution: None,
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
            execution: None,
        })
    }
    /// Select a stable durable task while idle; None disables persistence.
    /// The Agent retains the store. Rust borrowing rejects concurrent configuration.
    pub fn set_execution_store(
        &mut self,
        scope: &str,
        store: Option<SharedExecutionStore>,
    ) -> Result<(), Error> {
        let scope = cstring(scope)?;
        let mut holder = store.map(|store| Box::new(execution::StoreHolder(store)));
        let user = holder.as_mut().map_or(std::ptr::null_mut(), |holder| {
            (&mut **holder as *mut execution::StoreHolder).cast()
        });
        let callback = holder
            .as_ref()
            .map(|_| execution::callback as ffi::ExecutionFn);
        if unsafe {
            ffi::libai_agent_set_execution_store(self.pointer, scope.as_ptr(), callback, user)
        } != 0
        {
            return Err(native_error());
        }
        self.execution = holder;
        Ok(())
    }
    /// Copy the full idle transcript as a JSON array.
    pub fn messages(&self) -> Result<String, Error> {
        owned_result(unsafe { ffi::libai_agent_messages(self.pointer) })
    }
    /// Replace the agent capability profile while no run is active.
    /// Host-validate final JSON. Empty schema disables validation; feedback range 0..8.
    pub fn configure_output(&mut self, schema_json: &str, repairs: usize) -> Result<(), Error> {
        if repairs > 8 {
            return Err(Error("output feedback limit exceeds 8".into()));
        }
        let schema = cstring(schema_json)?;
        if unsafe { ffi::libai_agent_configure_output(self.pointer, schema.as_ptr(), repairs) } != 0
        {
            return Err(native_error());
        }
        Ok(())
    }
    pub fn configure_model(&mut self, profile_json: &str) -> Result<(), Error> {
        let profile = cstring(profile_json)?;
        if unsafe { ffi::libai_agent_configure_model(self.pointer, profile.as_ptr()) } != 0 {
            return Err(native_error());
        }
        Ok(())
    }
    pub fn add_tool(
        &mut self,
        name: &str,
        description: &str,
        schema: &str,
        requires_approval: bool,
        tool: impl FnMut(ToolCall, &Invocation<'_>) -> ToolResult + 'static,
    ) -> Result<&mut Self, Error> {
        self.add_tool_versioned(name, description, schema, requires_approval, "1", tool)
    }
    /// Register with a host execution version; change it when handler semantics change.
    pub fn add_tool_versioned(
        &mut self,
        name: &str,
        description: &str,
        schema: &str,
        requires_approval: bool,
        execution_revision: &str,
        tool: impl FnMut(ToolCall, &Invocation<'_>) -> ToolResult + 'static,
    ) -> Result<&mut Self, Error> {
        self.add_tool_governed(
            name,
            description,
            schema,
            requires_approval,
            execution_revision,
            false,
            tool,
        )
    }
    /// Trusted no-side-effect assertion allows replay of an unconfirmed handler.
    #[allow(clippy::too_many_arguments)]
    pub fn add_tool_governed(
        &mut self,
        name: &str,
        description: &str,
        schema: &str,
        requires_approval: bool,
        execution_revision: &str,
        side_effect_free: bool,
        tool: impl FnMut(ToolCall, &Invocation<'_>) -> ToolResult + 'static,
    ) -> Result<&mut Self, Error> {
        let revision = cstring(execution_revision)?;
        let name = cstring(name)?;
        let description = cstring(description)?;
        let schema = cstring(schema)?;
        let mut holder = Box::new(ToolHolder {
            callback: Box::new(tool),
        });
        let result = unsafe {
            ffi::libai_agent_add_tool_governed(
                self.pointer,
                name.as_ptr(),
                description.as_ptr(),
                schema.as_ptr(),
                requires_approval as i32,
                revision.as_ptr(),
                side_effect_free as i32,
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

/// Portable schema and governance operations; available without a desktop backend.
pub fn governance_call(operation: &str, arguments_json: &str) -> Result<String, Error> {
    let op = cstring(operation)?;
    let args = cstring(arguments_json)?;
    unsafe {
        let result = ffi::libai_governance_call(op.as_ptr(), args.as_ptr());
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
