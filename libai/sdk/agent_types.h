#pragma once

#include "task_contract.h"
#include "json_schema.h"
#include "model_profile.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <map>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace libai
{

// A provider-issued tool invocation with opaque JSON arguments.
struct tool_call_t {
	// Unique exchange identifier and registered application tool name.
	std::string id, name;
	// An opaque JSON object; the tool validates its own domain/schema.
	std::string arguments = "{}";
};

// One typed conversation item retained across agent runs.
struct message_t {
	enum class role_t { user, assistant, tool };
	// Wire role preserved during provider-specific transcript serialization.
	role_t role = role_t::user;
	// Visible text, tool-result correlation and optional reasoning replay.
	std::string text, call_id, reasoning;
	// Ordered native calls emitted by an assistant turn.
	std::vector<tool_call_t> calls;
	// Distinguish failed/denied tool results from successful outputs.
	bool tool_error = false;
};

// Application capability advertised to the model.
struct tool_spec_t {
	// Host version for recovery; change when handler semantics change.
	std::string execution_revision = "1";
	// Stable tool name and a description of its purpose.
	std::string name, description;
	// Complete JSON object schema; the handler validates its domain arguments.
	std::string parameters = "{\"type\":\"object\",\"properties\":{}}";
	// Require host authorization before executing this capability.
	bool requires_approval = false;
	// Trusted host assertion: handler has no externally observable side effects.
	// Allows replay of an unconfirmed invocation; never inferred from the model.
	bool side_effect_free = false;
};

// Thread-safe cooperative cancellation shared with transports and tools.
class cancellation_t {
public:
	// Request cancellation without interrupting an arbitrary application callback.
	void cancel()
	{
		cancelled_.store(true);
	}
	// Read the current cancellation request from any thread.
	bool cancelled() const
	{
		return cancelled_.load();
	}

private:
	// Atomic flag remains private to avoid unsynchronized application access.
	std::atomic<bool> cancelled_{ false };
};

// Provider-neutral request snapshot for one logical model attempt.
struct model_request_t {
    // Host-validated final JSON contract; no claim of provider-native enforcement.
    std::string output_schema;
    bool output_repair = false;
	// Explicit host capability profile applied to this request.
	model_profile_t profile;
	// Stable system instructions supplied by the application.
	std::string instructions;
	// Typed session history including assistant/tool exchanges.
	std::vector<message_t> messages;
	// Capabilities available to the current model turn.
	std::vector<tool_spec_t> tools;
	// Remaining per-request output token allowance.
	std::size_t max_output_tokens = 4096;
	// Cancellation boundary the model transport should observe.
	std::shared_ptr<cancellation_t> cancellation;
	// Deadline checked cooperatively by the model transport.
	std::chrono::steady_clock::time_point deadline =
	    std::chrono::steady_clock::time_point::max();
};

// Normalized completion or explicit retryable failure from a model.
struct model_response_t {
	// Visible completion and optional private reasoning replay.
	std::string text, reasoning;
	// Ordered tool invocations to validate before executing the batch.
	std::vector<tool_call_t> calls;
	// Reported provider token use; absent usage remains zero.
	std::size_t input_tokens = 0, output_tokens = 0;
	// Failure diagnostic; empty means the request succeeded.
	std::string error;
	// Only explicitly retryable model errors may be retried.
	bool retryable = false;
};

// Return false from a stream sink to ask the model transport to stop.
using stream_sink_t = std::function<bool(const std::string &)>;
// Pluggable transport interface independent of vendor SDKs.
class model_t {
public:
	// Permit safe destruction through the model interface.
	virtual ~model_t()
	{
	}
	// Complete a typed request and optionally forward streaming text.
	virtual model_response_t complete(
	    const model_request_t &, const stream_sink_t &) = 0;
};

// Adapt an application function to the model interface.
class function_model_t : public model_t {
public:
	using function_t = std::function<model_response_t(
	    const model_request_t &, const stream_sink_t &)>;
	// Retain a nonempty model callback or reject construction.
	explicit function_model_t(function_t function)
	        : function_(std::move(function))
	{
		if (!function_)
			throw std::invalid_argument("model callback is empty");
	}
	// Forward requests and stream sinks to the application backend.
	model_response_t complete(
	    const model_request_t &request, const stream_sink_t &sink) override
	{
		return function_(request, sink);
	}

private:
	// Application model function owned by the adapter.
	function_t function_;
};

// Execution boundary passed to each application tool handler.
struct tool_context_t {
	// Stable scope/ordinal key for externally idempotent operations.
	std::string idempotency_key;
	// Shared cancellation flag for long-running tool callbacks.
	std::shared_ptr<cancellation_t> cancellation;
	// Run deadline observed cooperatively by the tool.
	std::chrono::steady_clock::time_point deadline;
	// Report whether cancellation or the deadline requires stopping.
	bool stopped() const
	{
		return (cancellation && cancellation->cancelled()) ||
		    std::chrono::steady_clock::now() >= deadline;
	}
};

// Tool text and success/error state returned to the next model turn.
struct tool_result_t {
	// Opaque textual output supplied by the application.
	std::string text;
	// Mark failures, authorization denial and truncated results.
	bool error = false;
	// Construct a successful tool output.
	static tool_result_t success(const std::string &text)
	{
		tool_result_t result;
		result.text = text;
		return result;
	}
	// Construct a failed tool output without throwing through the loop.
	static tool_result_t failure(const std::string &text)
	{
		tool_result_t result;
		result.text = text;
		result.error = true;
		return result;
	}
};

using tool_handler_t =
    std::function<tool_result_t(const tool_call_t &, const tool_context_t &)>;
// Registered schema paired with an application execution callback.
struct tool_t {
	// Immutable shared schema compiled before model execution.
	std::shared_ptr<const json_schema_t> schema;
	// Capability declaration sent to the model.
	tool_spec_t spec;
	// Synchronous handler responsible for argument and domain validation.
	tool_handler_t execute;
};

// agent_session_t ownership is explicit; persist messages with an application codec.
// A session must not be used by simultaneous runs.
struct agent_session_t {
	// Application-owned transcript, including partial side effects.
	std::vector<message_t> messages;
};

enum class run_status_t {
	completed,
	cancelled,
	deadline_exceeded,
	budget_exceeded,
	model_error,
	protocol_error,
	callback_error,
	recovery_required
};
// Terminal state and accumulated accounting for one agent run.
struct run_result_t {
    bool output_validated = false;
    std::size_t output_repairs = 0;
	// Run termination and artifact acceptance are separate facts.
	task_contract_t task_contract;
	// Explicit lifecycle outcome independent of display text.
	run_status_t status = run_status_t::model_error;
	// Final answer and a separate terminal diagnostic.
	std::string text, error;
	// Logical model attempts, admitted handler attempts and reported token use.
	std::size_t model_calls = 0, tool_calls = 0, input_tokens = 0,
	            output_tokens = 0;
};

// Lightweight synchronous notification for model and tool phases.
struct agent_event_t {
	enum class kind_t {
		model_start,
		model_end,
		tool_start,
		tool_end,
		finished,
        tool_intent_committed,
        tool_result_committed,
        tool_result_reused
	};
	// Phase being observed on the current run thread.
	kind_t kind;
	// Tool identity and correlation for tool events; empty for model events.
	std::string tool_name, call_id;
	// Whether the phase reports an error or unsuccessful terminal state.
	bool error = false;
    // Snapshot facts, never an authorization or external receipt.
    std::string execution_scope, idempotency_key, journal_revision, effect_state;
    std::size_t attempts = 0;
	// Construct an event for the selected phase.
	explicit agent_event_t(kind_t value)
	        : kind(value)
	{
	}
};

// A store must hold an exclusive scope lease from open through close.
struct execution_snapshot_t { std::string revision, state_json; };
class execution_store_t {
public:
	virtual ~execution_store_t() {}
	virtual execution_snapshot_t open(const std::string &scope) = 0;
	// Durable compare-and-swap; throw on conflict/failure, return a fresh revision.
	virtual std::string commit(const std::string &scope, const std::string &expected_revision,
	    const std::string &state_json) = 0;
	virtual void close(const std::string &scope) noexcept = 0;
};

// Host-selected budgets, cancellation, authorization and presentation hooks.
struct run_options_t {
    std::string output_schema;
    std::size_t max_output_repairs = 1;
	// Optional durable store; scope is a stable host-owned task identifier.
	std::shared_ptr<execution_store_t> execution_store;
	std::string execution_scope;
	// Agent-owned capability profile; applied before model/tool callbacks.
	model_profile_t profile;
	// Hard limits on logical model attempts and executed tool handlers.
	std::size_t max_model_calls = 16, max_tool_calls = 64;
	// Reported output token budget and per-request generation ceiling.
	std::size_t max_output_tokens = 32768, max_tokens_per_call = 4096;
	// Text/schema context budget and bounded per-tool result size.
	std::size_t max_context_bytes = 1024 * 1024,
	            max_tool_result_bytes = 64 * 1024;
	// Maximum consecutive recoverable failures per logical model turn.
	std::size_t max_model_retries = 1;
	// Cooperative wall-clock deadline duration for the complete run.
	std::chrono::milliseconds timeout{ 300000 };
	// Optional caller-owned cancellation handle; otherwise allocated per run.
	std::shared_ptr<cancellation_t> cancellation;
	// Streaming text observer; returning false requests cancellation.
	stream_sink_t on_text;
	// Called for every tool. Missing callback denies approval-required tools.
	std::function<bool(const tool_spec_t &, const tool_call_t &)> authorize;
	// Synchronous event observer; application callbacks should not throw.
	std::function<void(const agent_event_t &)> on_event;
};

} // namespace libai
