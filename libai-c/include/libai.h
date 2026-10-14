#ifndef LIBAI_C_H
#define LIBAI_C_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(LIBAI_C_SHARED)
#ifdef LIBAI_C_BUILD
#define LIBAI_API __declspec(dllexport)
#else
#define LIBAI_API __declspec(dllimport)
#endif
#else
#define LIBAI_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque owner of a C++ agent, persistent session and registered callbacks. */
typedef struct libai_agent libai_agent;
/* Owning handle to a shareable cooperative stop token. */
typedef struct libai_cancellation libai_cancellation;
/* Borrowed model-output builder, valid only during a model callback. */
typedef struct libai_response libai_response;
/* Borrowed tool-output builder, valid only during a tool callback. */
typedef struct libai_tool_result libai_tool_result;
/* Borrowed cancellation/deadline/stream boundary for one callback. */
typedef struct libai_invocation libai_invocation;

/* Independent RAG component owner. No C++ or SDK types cross this ABI.
 * Handles/configuration/callbacks must not race destruction. All calls synchronous;
 * callback arguments/builders/invocations expire on return. See RAG.md wire format. */
typedef struct libai_rag libai_rag;
typedef struct libai_rag_result libai_rag_result;
/* operations: load/split/space_id/embed/replace_documents/search/retrieve.
 * space_id receives {} context and NULL invocation. All other operations inherit
 * trusted scope, deadline, cancellation and idempotency key. Callback must not throw. */
typedef void (*libai_rag_fn)(void *user,const char *operation,const char *arguments_json,
    const char *context_json,libai_rag_result *,libai_invocation *);
/* Successful creation transfers user ownership; final component/Agent release calls
 * release once. Failed creation leaves ownership with caller and never calls release. */
typedef void (*libai_rag_release_fn)(void *user);
LIBAI_API libai_rag *libai_rag_create(const char *kind,libai_rag_fn,void *user,
    libai_rag_release_fn,const char *configuration_json);
LIBAI_API libai_rag *libai_rag_create_splitter(const char *configuration_json);
LIBAI_API libai_rag *libai_rag_create_memory_store(const char *configuration_json);
/* Retains underlying embedding/store; outer input handles may be destroyed afterward. */
LIBAI_API libai_rag *libai_rag_create_vector_retriever(libai_rag *embedding,
    libai_rag *store,const char *configuration_json);
/* Set exactly one copied JSON response; duplicate/invalid results fail closed. */
LIBAI_API int libai_rag_result_set(libai_rag_result *,const char *,size_t bytes);
LIBAI_API void libai_rag_result_error(libai_rag_result *,const char *error);
/* Context: access_scope required, timeout_ms default 300000 (0 unlimited),
 * idempotency_key optional. NULL cancellation creates a fresh per-call token. */
LIBAI_API char *libai_rag_load(libai_rag *,const char *source,const char *context_json,
    const libai_cancellation *);
/* Explicit limits object (NULL defaults). Does not write any vector store. */
LIBAI_API char *libai_rag_prepare_index(libai_rag *splitter,libai_rag *embedding,
    const char *documents_json,const char *context_json,const char *limits_json,
    const libai_cancellation *);
/* Explicit effect; failure/cancellation may follow a write. No automatic retry. */
LIBAI_API int libai_rag_commit_index(libai_rag *store,const char *prepared_json,
    const char *context_json,const libai_cancellation *);
LIBAI_API char *libai_rag_retrieve(libai_rag *,const char *request_json,
    const char *context_json,const libai_cancellation *);
/* Busy/reentrant destruction rejected. Owning strings use libai_string_free. */
LIBAI_API int libai_rag_destroy(libai_rag *);
/* Agent retains retriever and its dependencies. Host fixes scope/filter/revision;
 * model cannot override them. Defaults require approval and forbid safe replay. */
LIBAI_API int libai_agent_add_retrieval_tool(libai_agent *,libai_rag *retriever,
    const char *name,const char *binding_json);

/* MCP connection. Destroy must not race calls using this handle; Agent bindings
 * retain the underlying connection after its outer handle is destroyed. */
typedef struct libai_mcp libai_mcp;
typedef struct libai_mcp_result libai_mcp_result;
/* operation=request/notify/close. Input/context/result/invocation are borrowed
 * only until return; close has no invocation. Callback must not throw. */
typedef void (*libai_mcp_transport_fn)(void *user, const char *operation,
    const char *wire, const char *context_json, libai_mcp_result *, libai_invocation *);
/* Called once after the last connection/Agent binding releases callback ownership.
 * Ownership transfers only when create succeeds. Must not throw. */
typedef void (*libai_mcp_release_fn)(void *user);
LIBAI_API libai_mcp *libai_mcp_create(libai_mcp_transport_fn, void *user, libai_mcp_release_fn);
/* Copy a bounded response or report a transport error while inside a callback. */
LIBAI_API int libai_mcp_result_response(libai_mcp_result *, const char *wire, size_t bytes);
LIBAI_API void libai_mcp_result_error(libai_mcp_result *, const char *error);
LIBAI_API int libai_mcp_stdio_available(void);
LIBAI_API libai_mcp *libai_mcp_create_stdio(const char *options_json);
/* initialize/list_tools/tool/call_tool with strict JSON arguments. Returned text
 * is owned by the caller (libai_string_free). NULL means libai_last_error. */
LIBAI_API char *libai_mcp_call(libai_mcp *, const char *operation, const char *arguments_json,
    const libai_cancellation *);
LIBAI_API int libai_mcp_close(libai_mcp *);
/* Release only this outer handle; explicit close affects all shared bindings. */
LIBAI_API int libai_mcp_destroy(libai_mcp *);
LIBAI_API int libai_agent_add_mcp_tool(libai_agent *, libai_mcp *, const char *remote_name,
    const char *local_name, const char *policy_json);

/* Owning POSIX file store. Never destroy concurrently with any API using this handle.
 * Keep it alive/usable for all registered callbacks; destroy rejects a live lease. */
typedef struct libai_file_execution_store libai_file_execution_store;
LIBAI_API int libai_file_execution_store_available(void);
/* Explicit plaintext choice: no encryption. Root is an absolute private directory. */
LIBAI_API libai_file_execution_store *libai_file_execution_store_create_plaintext(const char *root);
/* Owned UTF-8 JSON {revision,state_json}; NULL reports failure without acquiring a lease. */
LIBAI_API char *libai_file_execution_store_open(libai_file_execution_store *, const char *scope);
/* Open without requiring a JSON parser. Both out pointers are required and receive
 * independently owned UTF-8 strings (libai_string_free). Error returns -1 and NULLs. */
LIBAI_API int libai_file_execution_store_open_values(libai_file_execution_store *, const char *scope,
    char **revision, char **state_json);
/* Owned UTF-8 revision; NULL may mean a commit has become uncertain. */
LIBAI_API char *libai_file_execution_store_commit(libai_file_execution_store *, const char *scope,
    const char *expected_revision, const char *state_json);
LIBAI_API int libai_file_execution_store_close(libai_file_execution_store *, const char *scope);
LIBAI_API int libai_file_execution_store_destroy(libai_file_execution_store *);

/* Callback-local durable store response; strings are copied by the builder. */
typedef struct libai_execution_result libai_execution_result;
/* Operations: open, commit, close. Hold an exclusive scope lease until close. */
typedef void (*libai_execution_fn)(void *user, const char *operation, const char *scope,
    const char *expected_revision, const char *state_json, libai_execution_result *result);
/* Set revision/state_json/error during the store callback; no ownership transfer. */
LIBAI_API void libai_execution_result_set(libai_execution_result *, const char *revision,
    const char *state_json, const char *error);
/* Configure only while idle. NULL callback disables persistence; context must stay alive. */
LIBAI_API int libai_agent_set_execution_store(libai_agent *, const char *scope,
    libai_execution_fn callback, void *user);
/* Borrowed stable key, valid only during this tool callback; copy before returning. */
LIBAI_API const char *libai_invocation_idempotency_key(const libai_invocation *);
/* Trusted host reconciliation; verify the external receipt before calling. */
LIBAI_API int libai_execution_confirm(libai_execution_fn, void *user, const char *scope,
    const char *input, const char *instructions, const char *expected_revision,
    const char *key, const char *output, int tool_error, const char *outcome, const char *reference);
/* Trusted legacy migration after independently verifying remaining budget.
 * Only unfinished logs without time_budget; CAS required. Zero means unlimited.
 * Does not reconcile effects. Never expose to the model. */
LIBAI_API int libai_execution_adopt_deadline(libai_execution_fn, void *user, const char *scope,
    const char *input, const char *instructions, const char *expected_revision,
    int64_t remaining_ms, const char *reference);

/* Owning bounded DAG. Destroy must not race any use; active/reentrant calls fail. */
typedef struct libai_workflow libai_workflow;
/* Callback-local output; it and all inputs/invocation expire on return. */
typedef struct libai_workflow_result libai_workflow_result;
/* Synchronous on run thread by default; max_parallel>1 uses worker threads and
 * requires parallel_safe nodes with disjoint writes. context_json contains the
 * execution_scope for a child Agent; invocation has no business idempotency key. */
typedef void (*libai_workflow_node_fn)(void *user, const char *state_json,
    const char *context_json, libai_workflow_result *, libai_invocation *);
/* Called once on graph destruction after successful node registration. Ownership
 * of user transfers only on success; release must not throw or use the graph. */
typedef void (*libai_workflow_release_fn)(void *user);
LIBAI_API libai_workflow *libai_workflow_create(void);
/* Strict descriptor: name, writes(string array), execution_revision(default 1),
 * parallel_safe(default false), side_effect_free(default false). Host assertions. */
LIBAI_API int libai_workflow_add_node(libai_workflow *, const char *descriptor_json,
    libai_workflow_node_fn, void *user, libai_workflow_release_fn);
LIBAI_API int libai_workflow_add_edge(libai_workflow *, const char *from,
    const char *to, const char *label);
/* Set exactly once during callback; copied JSON contains patch_json(string,
 * default {}), route, status(default completed), error. Wire bound 16MiB;
 * patch bound 2MiB, route 32 bytes, error 4096 bytes. Invalid/missing output fails. */
LIBAI_API int libai_workflow_result_set(libai_workflow_result *, const char *json, size_t bytes);
/* Owned UTF-8 result JSON (libai_string_free), NULL on bridge/config failure.
 * Strict options: scope,max_parallel,max_node_calls,max_state_bytes,timeout_ms.
 * Optional store callback/user are borrowed through return, using existing store
 * operations. Use a separate lease owner from child Agents; scope required for
 * persistence. Cancellation handle must stay valid through this call. */
LIBAI_API char *libai_workflow_run(libai_workflow *, const char *initial_state_json,
    const char *options_json, libai_execution_fn store, void *store_user,
    const libai_cancellation *);
/* Trusted host reconciliation for a whole node, never an unrestricted model tool.
 * output_json follows callback output format; outcome is none/applied. Matching
 * CAS revision and externally verified receipt/reference are required. */
LIBAI_API int libai_workflow_confirm_node(libai_workflow *, libai_execution_fn,
    void *store_user, const char *scope, const char *initial_state_json,
    const char *expected_revision, const char *node, const char *outcome,
    const char *output_json, const char *reference);
LIBAI_API int libai_workflow_destroy(libai_workflow *);

/* Synchronous UTF-8 JSON request callback; fill the supplied output builder. */
typedef void (*libai_model_fn)(
    void *, const char *, libai_response *, libai_invocation *);
/* Synchronous tool callback; id/name/arguments are borrowed UTF-8 strings. */
typedef void (*libai_tool_fn)(void *, const char *, const char *, const char *,
    libai_tool_result *, libai_invocation *);
/* Return nonzero to continue streaming; zero requests cancellation. */
typedef int (*libai_text_fn)(void *, const char *);
/* Return nonzero to authorize; missing hook denies approval-required tools. */
typedef int (*libai_authorize_fn)(void *, const char *, const char *, int);
/* Receive a borrowed JSON event on the synchronous run thread. */
typedef void (*libai_event_fn)(void *, const char *);

/* Stable run limits; initialize with libai_options_default before use. */
typedef struct libai_options {
	/* Logical model attempts and executed tool callbacks. */
	size_t max_model_calls, max_tool_calls;
	/* Reported total and per-request output-token limits. */
	size_t max_output_tokens, max_tokens_per_call;
	/* Text/schema context and per-tool result byte ceilings. */
	size_t max_context_bytes, max_tool_result_bytes;
	/* Recoverable model retries within one logical turn. */
	size_t max_model_retries;
	/* Cooperative run deadline in milliseconds. */
	int64_t timeout_ms;
} libai_options;

/* Optional observers and authorization policy for a single run. */
typedef struct libai_hooks {
	/* Application context retained by the caller throughout the run. */
	void *user;
	/* Optional streaming observer. */
	libai_text_fn on_text;
	/* Optional per-tool authorization callback. */
	libai_authorize_fn authorize;
	/* Optional lifecycle-event observer. */
	libai_event_fn on_event;
} libai_hooks;

/* ABI version used to detect incompatible native/wrapper packages. */
/* Portable governance and schema operations; owned UTF-8 JSON, no desktop root needed. */
LIBAI_API char *libai_governance_call(const char *operation,const char *arguments);
LIBAI_API unsigned libai_abi_version(void);
/* Configure idle agent capabilities using a versioned JSON profile; returns 0 on success. */
LIBAI_API int libai_agent_configure_model(libai_agent *agent, const char *profile_json);
/* Idle configuration; empty schema disables validation. Feedback range 0..8. */
LIBAI_API int libai_agent_configure_output(libai_agent *agent, const char *schema_json, size_t repairs);
/* Thread-local diagnostic for a failed API call; never free this pointer. */
LIBAI_API const char *libai_last_error(void);
/* Initialize all run limits to the C++ SDK defaults. */
LIBAI_API void libai_options_default(libai_options *options);
/* Create an agent; instructions and callback outputs are copied by the bridge. */
LIBAI_API libai_agent *libai_agent_create(
    libai_model_fn model, void *user, const char *instructions);
/* Create an agent backed by the built-in desktop Provider transport.
 * config is a JSON object matching the C++ ProviderConfig fields; key is not stored.
 * Supports tool callbacks, streaming and shared cancellation through normal run APIs.
 * Portable builds return NULL and an availability diagnostic. */
LIBAI_API libai_agent *libai_agent_create_provider(
    const char *config, const char *api_key, const char *instructions);
/* Allocate an independent stop token, including before a run starts. */
LIBAI_API libai_cancellation *libai_cancellation_create(void);
/* Retain the same stop state in another independently owned handle. */
LIBAI_API libai_cancellation *libai_cancellation_retain(
    const libai_cancellation *);
/* Destroy one handle; retained handles and an active run keep the state alive.
 * Do not destroy a handle concurrently with an API call using that same handle. */
LIBAI_API void libai_cancellation_destroy(libai_cancellation *);
/* Request or inspect stop state; safe concurrently with runs using retained handles. */
LIBAI_API void libai_cancellation_cancel(libai_cancellation *);
/* Inspect the shared atomic stop state without modifying it. */
LIBAI_API int libai_cancellation_cancelled(const libai_cancellation *);
/* Copy an idle agent's full conversation into an owned UTF-8 JSON array. */
LIBAI_API char *libai_agent_messages(libai_agent *);
/* Run using a caller-owned token; NULL allocates a fresh per-run token.
 * The token must remain valid until this call returns; state is never reset. */
LIBAI_API char *libai_agent_run_with_cancellation(libai_agent *, const char *,
    const libai_options *, const libai_hooks *, const libai_cancellation *);
/* Destroy an idle agent; return -1 rather than freeing an active agent. */
LIBAI_API int libai_agent_destroy(libai_agent *agent);
/* Register a tool while idle; contexts must outlive the agent. */
LIBAI_API int libai_agent_add_tool(libai_agent *agent, const char *name,
    const char *description, const char *schema, int approval,
    libai_tool_fn tool, void *user);
/* Register with a host-owned execution semantics version (1..128 bytes).
 * Change it when handler semantics change; pending recovery rejects old versions. */
LIBAI_API int libai_agent_add_tool_versioned(libai_agent *agent, const char *name,
    const char *description, const char *schema, int approval, const char *execution_revision,
    libai_tool_fn tool, void *user);
/* Trusted host declaration, not a model parameter. side_effect_free permits safe
 * replay of an unconfirmed handler. Default APIs keep it false. */
LIBAI_API int libai_agent_add_tool_governed(libai_agent *agent, const char *name,
    const char *description, const char *schema, int approval, const char *execution_revision,
    int side_effect_free, libai_tool_fn tool, void *user);
/* Run synchronously; return owned UTF-8 result JSON or NULL on bridge failure. */
LIBAI_API char *libai_agent_run(libai_agent *agent, const char *input,
    const libai_options *options, const libai_hooks *hooks);
/* Cancel the current run; may run concurrently, but not concurrently with destroy. */
LIBAI_API void libai_agent_cancel(libai_agent *agent);
/* Clear an idle agent's in-memory session without removing its tools. */
LIBAI_API int libai_agent_reset(libai_agent *agent);
/* Release an owned result returned by libai_agent_run. */
LIBAI_API void libai_string_free(char *text);

/* Set copied final text and reasoning on a borrowed model output. */
LIBAI_API void libai_response_text(
    libai_response *, const char *, const char *);
/* Append a copied tool invocation; arguments must describe a JSON object. */
LIBAI_API void libai_response_call(
    libai_response *, const char *, const char *, const char *);
/* Report token usage; unknown usage is zero. */
LIBAI_API void libai_response_usage(libai_response *, size_t, size_t);
/* Report an explicit model failure and whether it may be retried. */
LIBAI_API void libai_response_error(libai_response *, const char *, int);
/* Set a copied tool output and its failure flag. */
LIBAI_API void libai_tool_result_set(libai_tool_result *, const char *, int);
/* Observe cooperative cancellation or deadline during a callback. */
LIBAI_API int libai_invocation_stopped(const libai_invocation *);
/* Call only while invocation is valid. Returned token is independently owned and
 * shares the parent stop state; NULL also means the parent has no stop token. */
LIBAI_API libai_cancellation *libai_invocation_retain_cancellation(const libai_invocation *);
/* Remaining cooperative deadline, -1 for unlimited, 0 for expired/invalid.
 * A child run must not turn 0 into its own unlimited timeout. Recheck stopped
 * before launch; the parent deadline cannot hard-interrupt a blocking callback. */
LIBAI_API int64_t libai_invocation_remaining_ms(const libai_invocation *);
/* Forward one UTF-8 model text delta; false means stop producing output. */
LIBAI_API int libai_invocation_emit(libai_invocation *, const char *);

/* Whether the library includes the optional desktop coding backend. */
LIBAI_API int libai_coding_available(void);
/* Root-scoped coding operation; returns owned JSON freed by libai_string_free.
 * NULL indicates ABI/availability errors; operation errors are {ok:false,error} JSON.
 * Host must authorize calls. Arguments are UTF-8 JSON objects. */
LIBAI_API char *libai_coding_call(
    const char *root, const char *operation, const char *arguments);

#ifdef __cplusplus
}
#endif
#endif
