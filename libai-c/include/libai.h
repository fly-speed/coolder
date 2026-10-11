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
LIBAI_API unsigned libai_abi_version(void);
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
