#pragma once
#include "export.h"

#include <cstddef>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace libai
{
namespace cpp
{

// A provider-issued tool invocation with opaque JSON arguments.
struct ToolCall {
	// Unique exchange identifier and registered application tool name.
	std::string id, name;
	// An opaque JSON object; the tool validates its own domain/schema.
	std::string arguments = "{}";
};

// One typed conversation item retained across agent runs.
struct Message {
	enum class role_t { user, assistant, tool };
	// Wire role preserved during provider-specific transcript serialization.
	role_t role = role_t::user;
	// Visible text, tool-result correlation and optional reasoning replay.
	std::string text, call_id, reasoning;
	// Ordered native calls emitted by an assistant turn.
	std::vector<ToolCall> calls;
	// Distinguish failed/denied tool results from successful outputs.
	bool tool_error = false;
};

// Application capability advertised to the model.
struct ToolSpec {
	// Stable tool name and a description of its purpose.
	std::string name, description;
	// Complete JSON object schema; the handler validates its domain arguments.
	std::string parameters = "{\"type\":\"object\",\"properties\":{}}";
	// Require host authorization before executing this capability.
	bool requires_approval = false;
};

// Thread-safe cooperative cancellation without exposing a backend handle.
class LIBAI_CPP_API Cancellation {
public:
	// Create a cancellation token usable across transports and tools.
	Cancellation();
	// Release the private backend token after its last user finishes.
	~Cancellation();
	// Cancellation tokens have stable identity and cannot be copied.
	Cancellation(const Cancellation &) = delete;
	// Prevent replacing a live shared token.
	Cancellation &operator=(const Cancellation &) = delete;
	// Request cancellation from any thread.
	void cancel() noexcept;
	// Observe cancellation from any thread.
	bool cancelled() const noexcept;

private:
	// Permit the implementation to translate a token without public native types.
	friend class Agent;
	// Permit only the compiled provider adapter to share cancellation state.
	friend class NativeProviderModel;
	// Backend-owned cancellation storage.
	struct Impl;
	// Retain the private token for this public object's lifetime.
	std::unique_ptr<Impl> impl_;
};

// Provider-neutral request snapshot for one logical model attempt.
struct ModelRequest {
	// Stable system instructions supplied by the application.
	std::string instructions;
	// Typed session history including assistant/tool exchanges.
	std::vector<Message> messages;
	// Capabilities available to the current model turn.
	std::vector<ToolSpec> tools;
	// Remaining per-request output token allowance.
	std::size_t max_output_tokens = 4096;
	// Cancellation boundary the model transport should observe.
	std::shared_ptr<Cancellation> cancellation;
	// Deadline checked cooperatively by the model transport.
	std::chrono::steady_clock::time_point deadline =
	    std::chrono::steady_clock::time_point::max();
};

// Normalized completion or explicit retryable failure from a model.
struct ModelResponse {
	// Visible completion and optional private reasoning replay.
	std::string text, reasoning;
	// Ordered tool invocations to validate before executing the batch.
	std::vector<ToolCall> calls;
	// Reported provider token use; absent usage remains zero.
	std::size_t input_tokens = 0, output_tokens = 0;
	// Failure diagnostic; empty means the request succeeded.
	std::string error;
	// Only explicitly retryable model errors may be retried.
	bool retryable = false;
};

// Return false from a stream sink to ask the model transport to stop.
using StreamSink = std::function<bool(const std::string &)>;
// Pluggable transport interface independent of vendor SDKs.
class Model {
public:
	// Permit safe destruction through the model interface.
	virtual ~Model()
	{
	}
	// Complete a typed request and optionally forward streaming text.
	virtual ModelResponse complete(
	    const ModelRequest &, const StreamSink &) = 0;
};

// Function-based application transport accepted directly by Agent.
using ModelFunction =
    std::function<ModelResponse(const ModelRequest &, const StreamSink &)>;

// Execution boundary passed to each application tool handler.
struct ToolContext {
	// Shared cancellation flag for long-running tool callbacks.
	std::shared_ptr<Cancellation> cancellation;
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
struct ToolResult {
	// Opaque textual output supplied by the application.
	std::string text;
	// Mark failures, authorization denial and truncated results.
	bool error = false;
	// Construct a successful tool output.
	static ToolResult success(const std::string &text)
	{
		ToolResult result;
		result.text = text;
		return result;
	}
	// Construct a failed tool output without throwing through the loop.
	static ToolResult failure(const std::string &text)
	{
		ToolResult result;
		result.text = text;
		result.error = true;
		return result;
	}
};

using ToolHandler =
    std::function<ToolResult(const ToolCall &, const ToolContext &)>;
enum class RunStatus {
	completed,
	cancelled,
	deadline_exceeded,
	budget_exceeded,
	model_error,
	protocol_error,
	callback_error
};
// Terminal state and accumulated accounting for one agent run.
struct RunResult {
	// Explicit lifecycle outcome independent of display text.
	RunStatus status = RunStatus::model_error;
	// Final answer and a separate terminal diagnostic.
	std::string text, error;
	// Logical model attempts, executed handlers and reported token use.
	std::size_t model_calls = 0, tool_calls = 0, input_tokens = 0,
	            output_tokens = 0;
};

// Lightweight synchronous notification for model and tool phases.
struct Event {
	enum class kind_t {
		model_start,
		model_end,
		tool_start,
		tool_end,
		finished
	};
	// Phase being observed on the current run thread.
	kind_t kind;
	// Tool identity and correlation for tool events; empty for model events.
	std::string tool_name, call_id;
	// Whether the phase reports an error or unsuccessful terminal state.
	bool error = false;
	// Construct an event for the selected phase.
	explicit Event(kind_t value)
	        : kind(value)
	{
	}
};

// Host-selected budgets, cancellation, authorization and presentation hooks.
struct RunOptions {
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
	std::shared_ptr<Cancellation> cancellation;
	// Streaming text observer; returning false requests cancellation.
	StreamSink on_text;
	// Called for every tool. Missing callback denies approval-required tools.
	std::function<bool(const ToolSpec &, const ToolCall &)> authorize;
	// Synchronous event observer; application callbacks should not throw.
	std::function<void(const Event &)> on_event;
};

} // namespace cpp
} // namespace libai
