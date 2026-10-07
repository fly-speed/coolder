#include "stdafx.h"
// Streaming observer, provider continuations and retry policy.
#include "coding_tool_loop_internal.h"

namespace action {
namespace agent_detail {

using coding_loop_detail::save_coding_progress;

class runtime_stream_observer_t
	: public webcool::ai::completion_stream_observer_t {
public:
	explicit runtime_stream_observer_t(
		const std::shared_ptr<agent_runtime_task_t>& task,
		const std::string& project_path)
		: task_(task), request_store_(task->user_root, project_path, task->id),
		  progress_store_(task->user_root, project_path, task->session_id) {}
	bool on_request_payload(const std::string& payload, std::string& err) {
		std::string relative_path;
		if (!request_store_.append(payload, relative_path, err)) {
			// The store already records the detailed ACL error. Add runtime context
			// without logging the payload, prompts, source code or image data.
			webcool::ai::ai_log_error("agent.runtime",
				"archive-provider-request", err);
			return false;
		}
		return true;
	}
	bool cancel_requested() const {
		return runtime_cancel_requested(task_);
	}
	void on_stream_progress(const webcool::ai::provider_stream_progress_t& progress) {
		{
			std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
			task_->stream_progress = progress;
			++task_->event_version;
		}
		acl::json json;
		acl::json_node& event = json.create_node();
		event.add_text("event", "model_stream_progress");
		event.add_text("phase", progress.phase.c_str());
		event.add_number("elapsed_ms", progress.elapsed_ms);
		event.add_number("headers_ms", progress.headers_ms);
		event.add_number("first_byte_ms", progress.first_byte_ms);
		event.add_number("last_data_ms", progress.last_data_ms);
		event.add_number("last_effective_ms", progress.last_effective_ms);
		event.add_number("received_bytes", static_cast<long long>(progress.received_bytes));
		event.add_number("text_bytes", static_cast<long long>(progress.text_bytes));
		event.add_number("reasoning_bytes", static_cast<long long>(progress.reasoning_bytes));
		event.add_number("tool_argument_bytes", static_cast<long long>(progress.tool_argument_bytes));
		append_runtime_operation_event(task_, event);
	}
	bool on_text_delta(const std::string& delta) {
		return append_runtime_model_delta(task_, delta);
	}
	bool on_reasoning_delta(const std::string& delta) {
		return append_runtime_reasoning_delta(task_, delta);
	}
	bool on_response_state(const std::string& response_id,
		const std::string& status) {
		// Polling often reports the same in_progress state for minutes. Persist only
		// state transitions so a long background response does not rewrite the
		// checkpoint twice per second.
		if (response_id == last_response_id_ && status == last_status_) return true;
		last_response_id_ = response_id;
		last_status_ = status;
		{
			std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
			if (task_->done || task_->cancel_requested) return false;
			task_->provider_response_id = response_id;
			task_->provider_response_pending = status != "failed"
				&& status != "cancelled" && status != "incomplete"
				&& !response_id.empty();
			task_->phase = status == "queued" ? "provider_queued"
				: "provider_background";
			++task_->event_version;
		}
		acl::json state_json;
		acl::json_node& state_event = state_json.create_node();
		state_event.add_text("event", "provider_response_state");
		state_event.add_text("status", status.c_str());
		state_event.add_bool("response_id_present", !response_id.empty());
		append_runtime_operation_event(task_, state_event);
		if (task_->text_preview) return true;
		// The checkpoint already exists before the request begins. Updating only
		// its opaque provider state here closes the crash window between OpenAI
		// accepting a background response and the tool loop consuming its result.
		webcool::ai::agent_progress_t progress;
		bool found = false;
		std::string checkpoint_err;
		if (progress_store_.load(progress, found, checkpoint_err) && found) {
			progress.provider_response_id = response_id;
			progress.provider_response_pending = status != "failed"
				&& status != "cancelled" && status != "incomplete"
				&& !response_id.empty();
			progress.provider_tool_outputs.clear();
			progress.updated_at = static_cast<long long>(time(NULL));
			if (!progress_store_.save(progress, checkpoint_err)) {
				webcool::ai::ai_log_error("agent.runtime",
					"checkpoint-background-response", checkpoint_err);
			}
		} else if (!checkpoint_err.empty()) {
			webcool::ai::ai_log_error("agent.runtime",
				"load-background-checkpoint", checkpoint_err);
		}
		return true;
	}
private:
	std::shared_ptr<agent_runtime_task_t> task_;
	webcool::ai::agent_request_store_t request_store_;
	webcool::ai::agent_progress_store_t progress_store_;
	std::string last_response_id_;
	std::string last_status_;
};

// A false return aborts the run. Provider failures remain in model_call_completed
// so request_model can apply its existing recovery policy.
bool coding_tool_loop_t::request_or_retrieve_model(coding_turn_t& turn, runtime_stream_observer_t& observer, bool& model_call_completed)
{
	if (provider.protocol == "openai_responses"
		&& provider_response_pending && !provider_response_id.empty())
	{
		// A previous process already paid for and started this response. Retrieve
		// it until terminal instead of issuing the same coding request again.
		const std::chrono::steady_clock::time_point poll_started =
			std::chrono::steady_clock::now();
		size_t poll = 0;
		size_t consecutive_poll_errors = 0;
		while (std::chrono::duration_cast<std::chrono::minutes>(
			std::chrono::steady_clock::now() - poll_started).count() < 30) {
			if (!wait_while_runtime_paused(runtime_task)) {
				webcool::ai::completion_result_t cancelled;
				std::string cancel_err;
				webcool::ai::provider_client_t::cancel_response(provider, api_key,
					provider_response_id, cancelled, cancel_err);
				err = "agent run cancelled";
				return false;
			}
			if (!webcool::ai::provider_client_t::retrieve_response(provider,
				api_key, provider_response_id, turn.output, err))
			{
				if (!turn.output.response_status.empty()) {
					observer.on_response_state(provider_response_id,
						turn.output.response_status);
				}
				if ((turn.output.retryable_error
					|| turn.output.error_category == webcool::ai::provider_error_network)
					&& consecutive_poll_errors++ < 5)
				{
					acl::fiber::delay(1000);
					continue;
				}
				break;
			}
			consecutive_poll_errors = 0;
			observer.on_response_state(provider_response_id,
				turn.output.response_status);
			if (turn.output.response_status != "queued"
				&& turn.output.response_status != "in_progress")
			{
				model_call_completed = true;
				if (!turn.output.reasoning.empty()) {
					observer.on_reasoning_delta(turn.output.reasoning);
				}
				if (!turn.output.text.empty()) observer.on_text_delta(turn.output.text);
				break;
			}
			const unsigned int delay_ms = static_cast<unsigned int>(
				std::min<size_t>(5000, 750 + poll * 250));
			acl::fiber::delay(delay_ms);
			++poll;
		}
		if (!model_call_completed && err.empty()) {
			err = "OpenAI background response polling timed out";
			turn.output.error_category = webcool::ai::provider_error_timeout;
			// The provider is still running this response. Leave the persisted ID
			// pending for an explicit resume; never create a duplicate request.
			turn.output.retryable_error = false;
		}
	} else {
		model_call_completed = webcool::ai::provider_client_t::complete(
			provider, api_key, turn.input, turn.output, err, &observer);
	}
	return true;
}

bool coding_tool_loop_t::retry_interrupted_stream(coding_turn_t& turn, bool& model_call_completed)
{
	append_model_operation_event(runtime_task,
		"model_stream_interrupted_before_retry", turn.output, err,
		turn.output.effective_max_output_tokens > 0
			? turn.output.effective_max_output_tokens : turn.input.max_output_tokens);
	// A provider may exceed the first-event idle timeout, or a proxy/TLS
	// connection may drop an established SSE stream. Save the exact tool-loop
	// checkpoint before one automatic retry so either failure resumes from the
	// latest completed workspace action instead of restarting the task.
	const webcool::ai::completion_result_t interrupted_output = turn.output;
	const std::string interrupted_err = err;
	std::string interrupted_reasoning =
		runtime_reasoning_snapshot(runtime_task);
	if (interrupted_reasoning.empty()) {
		interrupted_reasoning = accumulated_reasoning;
	}
	if (interrupted_reasoning.size() > 1024 * 1024) {
		interrupted_reasoning.resize(1024 * 1024);
	}
	std::string retry_save_err;
	if (!save_coding_progress(progress_store, provider, runtime_task,
		project_path, original_prompt, persistent_transcript,
		interrupted_reasoning, interrupted_err, turn.call, retry_save_err))
	{
		webcool::ai::ai_log_error("agent.runtime",
			"save-stream-retry-progress", retry_save_err);
	}
	accumulated_reasoning = interrupted_reasoning;
	webcool::ai::ai_log_error("agent.runtime",
		"retry-interrupted-model-stream", interrupted_err);

	// The transcript already contains every completed workspace tool call.
	// Retain the selected reasoning effort after a network interruption.
	// Carry the interrupted analysis as a checkpoint so the model can
	// continue without rediscovering completed work.
	if (!interrupted_reasoning.empty()) {
		const std::string checkpoint_prefix = prompt_text(prompt_id::transport_recovery, chinese);
		const std::string checkpoint_suffix =
			"\n</transport_recovery_checkpoint>";
		const size_t prompt_limit = webcool::ai::kMaxAgentPromptBytes - 8 * 1024;
		if (turn.input.user_prompt.size() + checkpoint_prefix.size()
			+ checkpoint_suffix.size() < prompt_limit)
		{
			const size_t available = prompt_limit - turn.input.user_prompt.size()
				- checkpoint_prefix.size() - checkpoint_suffix.size();
			const size_t retained = std::min<size_t>(8 * 1024,
				std::min(available, interrupted_reasoning.size()));
			turn.input.user_prompt += checkpoint_prefix;
			turn.input.user_prompt.append(interrupted_reasoning,
				interrupted_reasoning.size() - retained, retained);
			turn.input.user_prompt += checkpoint_suffix;
		}
	}
	// Do not reconnect immediately to the same unhealthy provider edge. The
	// delay is fiber-aware, so other users and runs continue normally.
	acl::json retry_event_json;
	acl::json_node& retry_event = retry_event_json.create_node();
	retry_event.add_text("event", "model_stream_retry_scheduled");
	retry_event.add_number("backoff_ms", 2000);
	retry_event.add_number("reasoning_checkpoint_bytes",
		static_cast<long long>(std::min<size_t>(8 * 1024,
			interrupted_reasoning.size())));
	append_runtime_operation_event(runtime_task, retry_event);
	acl::fiber::delay(2000);
	if (!wait_while_runtime_paused(runtime_task)) {
		err = "agent run cancelled";
		return false;
	}
	begin_runtime_model_stream(runtime_task);
	update_runtime_progress(runtime_task, "model_retry", "",
		executed_tool_calls);
	webcool::ai::completion_result_t retry_output;
	runtime_stream_observer_t retry_observer(runtime_task, project_path);
	std::string retry_err;
	model_call_completed = webcool::ai::provider_client_t::complete(
		provider, api_key, turn.input, retry_output, retry_err, &retry_observer);
	if (!wait_while_runtime_paused(runtime_task)) {
		err = "agent run cancelled";
		return false;
	}
	// Both requests may be billable. Preserve all usage and latency returned
	// by the provider while keeping failed-attempt reasoning only in the
	// separately accumulated reasoning transcript.
	retry_output.input_tokens += interrupted_output.input_tokens;
	retry_output.cached_input_tokens +=
		interrupted_output.cached_input_tokens;
	retry_output.output_tokens += interrupted_output.output_tokens;
	retry_output.reasoning_tokens += interrupted_output.reasoning_tokens;
	retry_output.latency_ms += interrupted_output.latency_ms;
	turn.output = retry_output;
	if (model_call_completed) {
		err.clear();
	} else {
		err = interrupted_err
			+ " Automatic retry from the saved tool-loop checkpoint also failed: "
			+ retry_err;
	}
	return true;
}

bool coding_tool_loop_t::request_model(coding_turn_t& turn)
{
	runtime_stream_observer_t observer(runtime_task, project_path);
	bool model_call_completed = false;
	if (!request_or_retrieve_model(turn, observer, model_call_completed)) return false;
	if (!model_call_completed) {
		const long long actual_output_limit = turn.output.effective_max_output_tokens > 0
			? turn.output.effective_max_output_tokens : turn.input.max_output_tokens;
		webcool::ai::annotate_output_token_limit(err,
			turn.output.incomplete_reason, actual_output_limit);
		// Record the original failure before retries can replace its details.
		// Error text may echo this user's credential, even without an sk- prefix.
		if (!api_key.empty()) {
			for (size_t pos = err.find(api_key); pos != std::string::npos;
				pos = err.find(api_key, pos + 10)) err.replace(pos, api_key.size(), "[REDACTED]");
		}
		append_model_operation_event(runtime_task,
			"model_attempt_failed", turn.output, err, actual_output_limit);
	}
	if (!model_call_completed && !turn.output.tool_arguments_recovery_attempted && tool_argument_recovery_attempts == 0
		&& err == "AI provider returned invalid tool arguments (expected JSON object)"
		&& !runtime_cancel_requested(runtime_task)) {
		++tool_argument_recovery_attempts;
		total_latency_ms += turn.output.latency_ms;
		total_input_tokens += turn.output.input_tokens;
		total_cached_input_tokens += turn.output.cached_input_tokens;
		total_output_tokens += turn.output.output_tokens;
		total_reasoning_tokens += turn.output.reasoning_tokens;
		append_model_operation_event(runtime_task, "tool_arguments_recovery", turn.output, err);
		turn.input.user_prompt += prompt_text(prompt_id::tool_arguments_repair, chinese);
		if (provider.protocol != "openai_responses")
			turn.input.system_prompt += prompt_text(prompt_id::tool_arguments_repair, chinese);
		err.clear();
		model_call_completed = webcool::ai::provider_client_t::complete(
			provider, api_key, turn.input, turn.output, err, &observer);
	}
	// Never discard a response that was already paid for. A pause requested
	// during provider I/O takes effect here, before parsing or executing the
	// returned tool call.
	if (!wait_while_runtime_paused(runtime_task)) {
		err = "agent run cancelled";
		return false;
	}
	if (!model_call_completed && turn.output.retryable_error
		&& !turn.output.transport_retry_attempted
		&& (turn.output.error_category == webcool::ai::provider_error_timeout
			|| (turn.output.error_category == webcool::ai::provider_error_network
				&& (err.find("cannot read AI provider streaming response")
					!= std::string::npos
					|| err.find("AI provider streaming request failed")
						!= std::string::npos)))
		&& !runtime_cancel_requested(runtime_task))
	{
		if (!retry_interrupted_stream(turn, model_call_completed)) return false;
	}
	if (!model_call_completed)
	{
		const long long actual_output_limit = turn.output.effective_max_output_tokens > 0
			? turn.output.effective_max_output_tokens : turn.input.max_output_tokens;
		webcool::ai::annotate_output_token_limit(err,
			turn.output.incomplete_reason, actual_output_limit);
		append_model_operation_event(runtime_task,
			"model_request_failed", turn.output, err, actual_output_limit);
		// Preserve whatever the provider emitted before the body read failed.
		// Some adapters cannot return it in `output`, while the stream observer
		// has already accumulated the same visible reasoning for the browser.
		std::string failed_reasoning = runtime_reasoning_snapshot(runtime_task);
		if (failed_reasoning.empty()) failed_reasoning = accumulated_reasoning;
		final_output = turn.output;
		final_output.reasoning = failed_reasoning;
		// The failed attempt does not replace usage from already completed
		// tool rounds. Providers may omit usage for an interrupted stream;
		// preserve all known counters without inventing missing tokens.
		final_output.input_tokens += total_input_tokens;
		final_output.cached_input_tokens += total_cached_input_tokens;
		final_output.output_tokens += total_output_tokens;
		final_output.reasoning_tokens += total_reasoning_tokens;
		final_output.latency_ms += total_latency_ms;
		std::string save_err;
		if (!save_coding_progress(progress_store, provider, runtime_task,
			project_path, original_prompt, persistent_transcript,
			failed_reasoning, err, turn.call, save_err))
		{
			webcool::ai::ai_log_error("agent.runtime",
				"save-failed-progress", save_err);
			err += " (additionally failed to save recovery progress: "
				+ save_err + ")";
		} else {
			mark_runtime_recovery_available(runtime_task,
				progress_store.relative_path());
			err += " Recovery progress was saved at "
				+ progress_store.relative_path()
				+ "; retry the same task in this session to continue.";
		}
		return false;
	}
	if (!wait_while_runtime_paused(runtime_task)) {
		err = "agent run cancelled";
		return false;
	}
	return true;
}

} // namespace agent_detail
} // namespace action
