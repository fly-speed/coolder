//! Rust-owned callbacks over the shared native graph engine, without a JSON crate.
use crate::{
    callbacks::{cstring, discard_panic, text},
    execution::{self, StoreHolder},
    ffi,
    mcp::quote,
    native_error, owned_result, Cancellation, Error, Invocation, Options, SharedExecutionStore,
};
use std::{
    cell::RefCell,
    ffi::{c_char, c_void},
    marker::PhantomData,
    panic::{catch_unwind, AssertUnwindSafe},
    rc::Rc,
    sync::Mutex,
    thread::{self, ThreadId},
};

/// Callback-local boundary. It cannot escape the callback or be sent to another thread.
pub struct WorkflowContext<'a> {
    pub invocation: Invocation<'a>,
    /// Stable namespace for a child Agent journal, not a business idempotency key.
    pub execution_scope: String,
}
impl WorkflowContext<'_> {
    pub fn stopped(&self) -> bool {
        self.invocation.stopped()
    }
    /// Retain cancellation and inherit the remaining deadline for a child Agent.
    /// Keep the returned token alive through the synchronous child run.
    pub fn agent_options(&self) -> Result<(Options, Cancellation), Error> {
        if self.stopped() {
            return Err(Error("workflow stopped before child Agent".into()));
        }
        let token = self.invocation.retain_cancellation()?;
        let remaining = self.invocation.remaining_ms();
        if remaining == 0 || self.stopped() {
            return Err(Error("workflow deadline expired before child Agent".into()));
        }
        let options = Options {
            timeout_ms: remaining.max(0),
            ..Options::default()
        };
        Ok((options, token))
    }
}
/// Owned node output; patch_json must encode an object and only declared writes.
#[derive(Debug, Clone)]
pub struct WorkflowNodeResult {
    pub patch_json: String,
    pub route: String,
    pub status: String,
    pub error: String,
}
impl Default for WorkflowNodeResult {
    fn default() -> Self {
        Self {
            patch_json: "{}".into(),
            route: String::new(),
            status: "completed".into(),
            error: String::new(),
        }
    }
}
impl WorkflowNodeResult {
    fn json(&self) -> String {
        format!(
            "{{\"patch_json\":{},\"route\":{},\"status\":{},\"error\":{}}}",
            quote(&self.patch_json),
            quote(&self.route),
            quote(&self.status),
            quote(&self.error)
        )
    }
}
/// Safety assertions are host-owned. Parallel safety is selected by registration method.
pub struct WorkflowNodeSpec<'a> {
    pub name: &'a str,
    pub writes: &'a [&'a str],
    pub execution_revision: &'a str,
    pub side_effect_free: bool,
}
impl<'a> WorkflowNodeSpec<'a> {
    pub fn new(name: &'a str) -> Self {
        Self {
            name,
            writes: &[],
            execution_revision: "1",
            side_effect_free: false,
        }
    }
    fn json(&self, parallel: bool) -> String {
        let writes = self
            .writes
            .iter()
            .map(|key| quote(key))
            .collect::<Vec<_>>()
            .join(",");
        format!("{{\"name\":{},\"writes\":[{}],\"execution_revision\":{},\"side_effect_free\":{},\"parallel_safe\":{}}}",
            quote(self.name),writes,quote(self.execution_revision),self.side_effect_free,parallel)
    }
}
/// Store callbacks execute on the graph's caller thread, using a separate owner from child Agents.
pub struct WorkflowOptions<'a> {
    pub scope: &'a str,
    pub execution_store: Option<SharedExecutionStore>,
    pub cancellation: Option<&'a Cancellation>,
    pub max_parallel: usize,
    pub max_node_calls: usize,
    pub max_state_bytes: usize,
    pub timeout_ms: u64,
}
impl Default for WorkflowOptions<'_> {
    fn default() -> Self {
        Self {
            scope: "",
            execution_store: None,
            cancellation: None,
            max_parallel: 1,
            max_node_calls: 64,
            max_state_bytes: 2 * 1024 * 1024,
            timeout_ms: 300000,
        }
    }
}
impl WorkflowOptions<'_> {
    fn json(&self) -> String {
        format!("{{\"scope\":{},\"max_parallel\":{},\"max_node_calls\":{},\"max_state_bytes\":{},\"timeout_ms\":{}}}",
        quote(self.scope),self.max_parallel,self.max_node_calls,self.max_state_bytes,self.timeout_ms)
    }
}
/// Whole-node receipt independently verified by the host, never a model tool.
pub struct WorkflowReceipt<'a> {
    pub scope: &'a str,
    pub initial_state_json: &'a str,
    pub expected_revision: &'a str,
    pub node: &'a str,
    pub outcome: &'a str,
    pub output: &'a WorkflowNodeResult,
    pub reference: &'a str,
}
type LocalHandler = dyn FnMut(&str, &WorkflowContext<'_>) -> Result<WorkflowNodeResult, Error>;
type ParallelHandler =
    dyn FnMut(&str, &WorkflowContext<'_>) -> Result<WorkflowNodeResult, Error> + Send;
enum Holder {
    Local {
        caller: ThreadId,
        handler: RefCell<Box<LocalHandler>>,
    },
    Parallel(Mutex<Box<ParallelHandler>>),
}
// Native JSON metadata is a single ASCII identifier generated from validated scope/node names.
// Reject unexpected encoding instead of adding an implicit lossy JSON decoder.
fn execution_scope(metadata: &str) -> Result<String, Error> {
    let scope = metadata
        .strip_prefix("{\"execution_scope\":\"")
        .and_then(|s| s.strip_suffix("\"}"))
        .ok_or_else(|| Error("workflow context metadata invalid".into()))?;
    if scope.len() > 102
        || !scope
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-')
    {
        return Err(Error("workflow execution scope invalid".into()));
    }
    Ok(scope.into())
}
unsafe extern "C" fn callback(
    user: *mut c_void,
    state: *const c_char,
    metadata: *const c_char,
    result: *mut c_void,
    pointer: *mut c_void,
) {
    let response = catch_unwind(AssertUnwindSafe(|| -> Result<(), Error> {
        if pointer.is_null() {
            return Err(Error("workflow invocation missing".into()));
        }
        let holder = &*(user as *const Holder);
        let context = WorkflowContext {
            invocation: Invocation {
                pointer,
                lifetime: PhantomData,
            },
            execution_scope: execution_scope(&text(metadata))?,
        };
        let input = text(state);
        let output = match holder {
            Holder::Local { caller, handler } => {
                // Fail before touching thread-local captured state if the native
                // executor ever violates its serial-node scheduling contract.
                if *caller != thread::current().id() {
                    return Err(Error("local workflow callback on another thread".into()));
                }
                handler
                    .try_borrow_mut()
                    .map_err(|_| Error("workflow callback busy".into()))?(
                    &input, &context
                )?
            }
            Holder::Parallel(handler) => handler
                .try_lock()
                .map_err(|_| Error("workflow callback busy or poisoned".into()))?(
                &input, &context,
            )?,
        };
        let wire = cstring(&output.json())?;
        if ffi::libai_workflow_result_set(result, wire.as_ptr(), wire.as_bytes().len()) != 0 {
            return Err(Error(
                "workflow callback output invalid or too large".into(),
            ));
        }
        Ok(())
    }));
    let error = match response {
        Ok(Ok(())) => return,
        Ok(Err(e)) => e.0,
        Err(payload) => {
            discard_panic(payload);
            "Rust workflow callback panicked".into()
        }
    };
    let mut error = error.replace('\0', "");
    if error.len() > 4096 {
        let mut end = 4096;
        while !error.is_char_boundary(end) {
            end -= 1;
        }
        error.truncate(end);
    }
    if error.is_empty() {
        error = "Rust workflow callback failed".into();
    }
    let output = WorkflowNodeResult {
        error,
        ..Default::default()
    };
    if let Ok(wire) = cstring(&output.json()) {
        ffi::libai_workflow_result_set(result, wire.as_ptr(), wire.as_bytes().len());
    }
}
unsafe extern "C" fn release(user: *mut c_void) {
    if let Err(payload) = catch_unwind(AssertUnwindSafe(|| {
        drop(Box::from_raw(user as *mut Holder))
    })) {
        discard_panic(payload);
    }
}
/// Same-thread owning DAG. Only add_parallel_node admits Send callbacks to native workers.
/// Do not leak an Rc cycle through a callback; use explicit close to break ownership cycles.
///
/// ```compile_fail
/// use libai::{Workflow,WorkflowNodeSpec,WorkflowNodeResult};
/// use std::{rc::Rc,cell::Cell};
/// let state=Rc::new(Cell::new(0));
/// let mut graph=Workflow::new().unwrap();
/// graph.add_parallel_node(WorkflowNodeSpec::new("a"),move |_,_|{
///     state.set(1);Ok(WorkflowNodeResult::default())
/// }).unwrap(); // Rc<Cell<_>> is not Send.
/// ```
pub struct Workflow {
    pointer: *mut c_void,
    _local: PhantomData<Rc<()>>,
}
impl Workflow {
    pub fn new() -> Result<Self, Error> {
        if unsafe { ffi::libai_abi_version() } != 1 {
            return Err(Error("unsupported native ABI".into()));
        }
        let pointer = unsafe { ffi::libai_workflow_create() };
        if pointer.is_null() {
            return Err(native_error());
        }
        Ok(Self {
            pointer,
            _local: PhantomData,
        })
    }
    fn check(&self) -> Result<(), Error> {
        if self.pointer.is_null() {
            Err(Error("workflow is closed".into()))
        } else {
            Ok(())
        }
    }
    fn register(&mut self, spec: WorkflowNodeSpec<'_>, holder: Holder) -> Result<&mut Self, Error> {
        self.check()?;
        let descriptor = cstring(&spec.json(matches!(&holder, Holder::Parallel(_))))?;
        let user = Box::into_raw(Box::new(holder));
        let result = unsafe {
            ffi::libai_workflow_add_node(
                self.pointer,
                descriptor.as_ptr(),
                callback,
                user.cast(),
                release,
            )
        };
        if result != 0 {
            let error = native_error();
            unsafe {
                drop(Box::from_raw(user));
            }
            return Err(error);
        }
        Ok(self)
    }
    pub fn add_node(
        &mut self,
        spec: WorkflowNodeSpec<'_>,
        handler: impl FnMut(&str, &WorkflowContext<'_>) -> Result<WorkflowNodeResult, Error> + 'static,
    ) -> Result<&mut Self, Error> {
        self.register(
            spec,
            Holder::Local {
                caller: thread::current().id(),
                handler: RefCell::new(Box::new(handler)),
            },
        )
    }
    pub fn add_parallel_node(
        &mut self,
        spec: WorkflowNodeSpec<'_>,
        handler: impl FnMut(&str, &WorkflowContext<'_>) -> Result<WorkflowNodeResult, Error>
            + Send
            + 'static,
    ) -> Result<&mut Self, Error> {
        self.register(spec, Holder::Parallel(Mutex::new(Box::new(handler))))
    }
    pub fn add_edge(&mut self, from: &str, to: &str, label: &str) -> Result<&mut Self, Error> {
        self.check()?;
        let from = cstring(from)?;
        let to = cstring(to)?;
        let label = cstring(label)?;
        if unsafe {
            ffi::libai_workflow_add_edge(self.pointer, from.as_ptr(), to.as_ptr(), label.as_ptr())
        } != 0
        {
            return Err(native_error());
        }
        Ok(self)
    }
    /// Owned JSON result: status/state_json/error/nodes/node_calls/progress_available/journal_revision/task_contract.
    pub fn run(&mut self, initial: &str, options: &WorkflowOptions<'_>) -> Result<String, Error> {
        self.check()?;
        let initial = cstring(initial)?;
        let wire = cstring(&options.json())?;
        let mut holder = options
            .execution_store
            .as_ref()
            .map(|s| StoreHolder(Rc::clone(s)));
        let user = holder
            .as_mut()
            .map_or(std::ptr::null_mut(), |h| (h as *mut StoreHolder).cast());
        let store = holder
            .as_ref()
            .map(|_| execution::callback as ffi::ExecutionFn);
        let fresh = if options.cancellation.is_none() {
            Some(Cancellation::new()?)
        } else {
            None
        };
        let cancellation = options.cancellation.or(fresh.as_ref()).unwrap();
        owned_result(unsafe {
            ffi::libai_workflow_run(
                self.pointer,
                initial.as_ptr(),
                wire.as_ptr(),
                store,
                user,
                cancellation.pointer,
            )
        })
    }
    pub fn confirm_node(
        &mut self,
        store: SharedExecutionStore,
        receipt: WorkflowReceipt<'_>,
    ) -> Result<(), Error> {
        self.check()?;
        let mut holder = StoreHolder(store);
        let scope = cstring(receipt.scope)?;
        let initial = cstring(receipt.initial_state_json)?;
        let revision = cstring(receipt.expected_revision)?;
        let node = cstring(receipt.node)?;
        let outcome = cstring(receipt.outcome)?;
        let output = cstring(&receipt.output.json())?;
        let reference = cstring(receipt.reference)?;
        if unsafe {
            ffi::libai_workflow_confirm_node(
                self.pointer,
                execution::callback,
                (&mut holder as *mut StoreHolder).cast(),
                scope.as_ptr(),
                initial.as_ptr(),
                revision.as_ptr(),
                node.as_ptr(),
                outcome.as_ptr(),
                output.as_ptr(),
                reference.as_ptr(),
            )
        } != 0
        {
            return Err(native_error());
        }
        Ok(())
    }
    pub fn close(&mut self) -> Result<(), Error> {
        if self.pointer.is_null() {
            return Ok(());
        }
        if unsafe { ffi::libai_workflow_destroy(self.pointer) } != 0 {
            return Err(native_error());
        }
        self.pointer = std::ptr::null_mut();
        Ok(())
    }
}
impl Drop for Workflow {
    fn drop(&mut self) {
        let _ = self.close();
    }
}
