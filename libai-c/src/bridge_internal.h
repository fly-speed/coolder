#pragma once
#include "libai.h"
#include <libai/agent.h>
#include <mutex>

/* Callback-local view of the native model result. */
struct libai_response {
	/* The C++ result whose strings and calls are copied from foreign code. */
	libai::model_response_t *value;
	/* Remember allocation failures without throwing through a foreign callback. */
	bool failed = false;
};
/* Callback-local view of the native tool result. */
struct libai_tool_result {
	/* The C++ output whose text is copied before the callback returns. */
	libai::tool_result_t *value;
	/* Remember allocation failures without throwing through a foreign callback. */
	bool failed = false;
};
/* A borrowed execution boundary; foreign wrappers must expire it on return. */
struct libai_invocation {
	/* Key copied from the tool admission checkpoint; empty for model callbacks. */
	std::string idempotency_key;
	/* Cancellation shared by model transport and tool callbacks. */
	std::shared_ptr<libai::cancellation_t> cancellation;
	/* Deadline inherited from the synchronous run. */
	std::chrono::steady_clock::time_point deadline;
	/* Text sink present only during model callbacks. */
	libai::stream_sink_t sink;
};
/* A native agent with one serialized session and cancellation independent of its lock. */
struct libai_agent {
	/* Idle-configured durable store and host-owned task scope. */
	std::shared_ptr<libai::execution_store_t> execution_store;
	std::string execution_scope;
	/* Guard configuration and the transcript against simultaneous runs. */
	std::mutex mutex;
	/* Atomic shared_ptr operations protect concurrent cancellation snapshots. */
	std::shared_ptr<libai::cancellation_t> cancellation;
	/* Callback model owned for the lifetime of the facade. */
	std::shared_ptr<libai::model_t> model;
	/* Generic C++ runtime containing the tool registry. */
	std::unique_ptr<libai::agent_t> runtime;
	/* Session retained across calls until reset or destruction. */
	libai::agent_session_t session;
};

/* Copy callback input without taking ownership of foreign memory. */
std::string bridge_text(const char *text);
/* Report a C API diagnostic in storage local to the calling thread. */
void bridge_error(const char *text) noexcept;
/* Serialize a model request without a platform JSON dependency. */
std::string bridge_request_json(const libai::model_request_t &request);
/* Serialize terminal state and accounting into portable UTF-8 JSON. */
std::string bridge_result_json(const libai::run_result_t &result);
/* Serialize a lifecycle event without exposing request bodies or credentials. */
std::string bridge_event_json(const libai::agent_event_t &event);

/* Shared native cancellation state behind one owning C handle. */
struct libai_cancellation {
	/* Retained SDK token; independently owned handles may share this state. */
	std::shared_ptr<libai::cancellation_t> value;
};
/* Copy a string into C-owned memory, throwing on allocation failure. */
char *bridge_owned(const std::string &text);
/* Serialize an ordered transcript independently of model request metadata. */
std::string bridge_messages_json(const std::vector<libai::message_t> &messages);

/* Store response owned by the native stack during a synchronous callback. */
struct libai_execution_result {
    std::string revision, state_json, error;
    bool failed=false;
};

/* Serialize a copied snapshot for file store consumers. */
std::string bridge_execution_snapshot_json(const libai::execution_snapshot_t &);
/* Shared synchronous adapter; the caller retains callback/user through use. */
std::shared_ptr<libai::execution_store_t> bridge_execution_store(libai_execution_fn, void *);
