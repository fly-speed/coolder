#include "stdafx.h"
#include "coding_runtime.h"
#include "../context/requirement_progress.h"
namespace action
{
namespace agent_detail
{
std::string operation_log_path(
    const std::shared_ptr<agent_runtime_task_t> &task)
{
	const std::string filename = "ai-operations-" + task->id + ".jsonl";
	return task->project_path.empty() ?
	    ".webcool_agent/" + filename :
	    task->project_path + "/.webcool_agent/" + filename;
}

std::string sanitize_operation_detail(const std::string &input)
{
	// Provider diagnostics are valuable, but some gateways echo bearer tokens or
	// provider-specific sk-/ak- keys inside an error. Redact those token-shaped
	// substrings before a diagnostic leaves memory.
	std::string output;
	output.reserve(std::min<size_t>(input.size(), 2048));
	for (size_t i = 0; i < input.size() && output.size() < 2048;) {
		size_t prefix_length = 0;
		const bool bearer = input.compare(i, 7, "Bearer ") == 0;
		const bool key_prefix =
		    (i == 0 ||
		        !std::isalnum(
		            static_cast<unsigned char>(input[i - 1]))) &&
		    (input.compare(i, 3, "sk-") == 0 ||
		        input.compare(i, 3, "ak-") == 0);
		if (bearer)
			prefix_length = 7;
		else if (key_prefix)
			prefix_length = 3;
		size_t token_end = i + prefix_length;
		while (prefix_length != 0 && token_end < input.size()) {
			const unsigned char c =
			    static_cast<unsigned char>(input[token_end]);
			if (std::isspace(c) || c == ']' || c == '>' ||
			    c == '"' || c == '\'')
				break;
			++token_end;
		}
		// Avoid mistaking ordinary words ending in "sk-" for a credential.
		if (key_prefix && token_end - (i + prefix_length) < 8) {
			prefix_length = 0;
		}
		if (prefix_length == 0) {
			const unsigned char c =
			    static_cast<unsigned char>(input[i++]);
			output.push_back(
			    c < 0x20 || c == 0x7f ? ' ' : static_cast<char>(c));
			continue;
		}
		output += "[redacted]";
		i = token_end;
	}
	return webcool::ai::utf8_prefix(output, 2048);
}

void append_runtime_operation_events(
    const std::shared_ptr<agent_runtime_task_t> &task,
    const std::vector<acl::json_node *> &events)
{
	if (!task)
		return;
	// The run ID and timestamp make lines independently searchable after several
	// users run agents concurrently. The filename is derived only from the
	// server-generated run ID and the already validated project path.
	const long long timestamp_ms = static_cast<long long>(
	    std::chrono::duration_cast<std::chrono::milliseconds>(
	        std::chrono::system_clock::now().time_since_epoch())
	        .count());
	std::string line;
	for (auto *event : events) {
		event->add_text("run_id", task->id.c_str());
		event->add_number("timestamp_ms", timestamp_ms);
		line += serialize_json(*event) + "\n";
	}

	std::lock_guard<webcool::mutex> guard(task->operation_log_mutex);
	if (!task->operation_log_initialized) {
		task->operation_log_initialized = true;
		if (task->resumed_after_restart) {
			webcool::ai::agent_workspace_t workspace(
			    task->user_root);
			std::string existing;
			bool truncated = false;
			std::string load_err;
			webcool::ai::agent_request_store_t log_store(
			    task->user_root, task->project_path, task->id);
			bool loaded =
			    log_store.operation_log(existing, false, load_err);
			if (!loaded) {
				const std::string legacy =
				    (task->project_path.empty() ?
				            "" :
				            task->project_path + "/") +
				    "ai-operations-" + task->id + ".jsonl";
				loaded = workspace.read(
				    legacy, existing, truncated, load_err);
			}
			if (loaded && !truncated) {
				task->operation_log = existing;
			} else if (!load_err.empty() &&
			    load_err.find("does not exist") ==
			        std::string::npos) {
				webcool::ai::ai_log_error("agent.operation-log",
				    "load-before-recovery", load_err);
			}
		}
	}
	if (task->operation_log_truncated)
		return;
	const bool creates_log_file = task->operation_log.empty();
	if (task->operation_log.size() + line.size() > kMaxOperationLogBytes) {
		acl::json truncated_json;
		acl::json_node &truncated = truncated_json.create_node();
		truncated.add_text("event", "operation_log_truncated");
		truncated.add_text("run_id", task->id.c_str());
		truncated.add_number("timestamp_ms", timestamp_ms);
		truncated.add_number("retained_bytes",
		    static_cast<long long>(task->operation_log.size()));
		line = serialize_json(truncated) + "\n";
		task->operation_log_truncated = true;
	}
	task->operation_log += line;

	webcool::ai::agent_request_store_t log_store(
	    task->user_root, task->project_path, task->id);
	std::string err;
	const std::string relative_path = operation_log_path(task);
	if (!log_store.operation_log(task->operation_log, true, err)) {
		// Do not recursively add a failed-log-write event to the same file.
		webcool::ai::ai_log_error(
		    "agent.operation-log", "persist", err);
	} else if (creates_log_file) {
		// Publish only the creation as a tree mutation. Subsequent appends need not
		// force a full project-tree refresh for every model delta/tool boundary.
		std::lock_guard<webcool::mutex> runtime_guard(
		    g_agent_runtime_mutex);
		++task->workspace_mutation_version;
		task->last_workspace_mutation_path = relative_path;
		++task->event_version;
	}
}

void append_runtime_operation_event(
    const std::shared_ptr<agent_runtime_task_t> &task, acl::json_node &event)
{
	append_runtime_operation_events(task, { &event });
}

void append_simple_operation_event(
    const std::shared_ptr<agent_runtime_task_t> &task, const char *name,
    const std::string &phase, const std::string &tool,
    size_t completed_tool_calls)
{
	acl::json json;
	acl::json_node &event = json.create_node();
	event.add_text("event", name);
	if (!phase.empty())
		event.add_text("phase", phase.c_str());
	if (!tool.empty())
		event.add_text("tool", tool.c_str());
	event.add_number("completed_tool_calls",
	    static_cast<long long>(completed_tool_calls));
	append_runtime_operation_event(task, event);
}

void append_model_operation_event(
    const std::shared_ptr<agent_runtime_task_t> &task, const char *name,
    const webcool::ai::completion_result_t &output, const std::string &error,
    long long effective_max_output_tokens,
    const webcool::ai::completion_result_t *attempt_usage)
{
	acl::json json;
	acl::json_node &event = json.create_node();
	event.add_text("event", name);
	webcool::ai::agent_protocol_message_t message;
	const bool parsed = webcool::ai::parse_agent_protocol_message(
	    output.text, message);
	const bool protocol = parsed || message.final_message;
	const std::string reply = protocol ? message.final_text : output.text;
	event.add_text("reply_overview",
	    sanitize_operation_detail(reply).c_str());
	event.add_bool("reply_overview_truncated", reply.size() > 2048);
	if (protocol) {
		event.add_text("completion_summary",
		    sanitize_operation_detail(message.completion_summary).c_str());
		if (!message.requirement_progress_json.empty())
			event.add_text("requirement_overview",
			    sanitize_operation_detail(
			        webcool::ai::requirement_progress_summary(
			            message.requirement_progress_json,
			            task->ui_language != "en")).c_str());
	}
	std::string tools;
	for (const auto &call : output.tool_calls) {
		if (!tools.empty()) tools += ", ";
		tools += call.name;
	}
	if (tools.empty()) tools = protocol ? message.tool.name : output.tool_name;
	event.add_text("requested_tools", sanitize_operation_detail(tools).c_str());
	const auto &usage = attempt_usage ? *attempt_usage : output;
	event.add_number("latency_ms", usage.latency_ms);
	event.add_number("input_tokens", usage.input_tokens);
	event.add_number("cached_input_tokens", usage.cached_input_tokens);
	event.add_bool("cache_usage_available", usage.cache_usage_available);
	event.add_number("output_tokens", usage.output_tokens);
	event.add_number("reasoning_tokens", usage.reasoning_tokens);
	if (effective_max_output_tokens > 0)
		event.add_number(
		    "effective_max_output_tokens", effective_max_output_tokens);
	event.add_number(
	    "visible_text_bytes", static_cast<long long>(output.text.size()));
	event.add_number(
	    "reasoning_bytes", static_cast<long long>(output.reasoning.size()));
	size_t tool_content_bytes = 0;
	for (const auto &call : output.tool_calls)
		tool_content_bytes += call.content.size();
	event.add_number(
	    "tool_content_bytes", static_cast<long long>(tool_content_bytes));
	event.add_bool("tool_arguments_recovery_attempted",
	    output.tool_arguments_recovery_attempted);
	event.add_bool("verbose_tool_preamble",
	    !output.tool_calls.empty() && output.text.size() > 4096);
	event.add_number("tool_call_count",
	    static_cast<long long>(output.tool_calls.size()));
	event.add_text("provider_error_category",
	    webcool::ai::provider_client_t::error_category_name(
	        output.error_category));
	event.add_bool("retryable", output.retryable_error);
	event.add_bool(
	    "transport_retry_attempted", output.transport_retry_attempted);
	event.add_bool("connection_reused", output.connection_reused);
	event.add_bool("native_tool_call", output.native_tool_call);
	event.add_bool(
	    "reasoning_budget_recovered", output.reasoning_budget_recovered);
	if (output.http_status > 0) {
		event.add_number("http_status", output.http_status);
	}
	if (!output.response_status.empty()) {
		event.add_text(
		    "response_status", output.response_status.c_str());
	}
	if (!output.incomplete_reason.empty()) {
		event.add_text("incomplete_reason",
		    sanitize_operation_detail(output.incomplete_reason)
		        .c_str());
	}
	if (!error.empty()) {
		event.add_text(
		    "error", sanitize_operation_detail(error).c_str());
		if (!output.error_response_excerpt.empty()) {
			event.add_text("error_response_excerpt",
			    sanitize_operation_detail(
			        output.error_response_excerpt)
			        .c_str());
			event.add_bool("error_response_excerpt_truncated",
			    output.error_response_excerpt.size() >= 2048);
		}
		if (!output.error_response_content_type.empty())
			event.add_text("error_response_content_type",
			    sanitize_operation_detail(
			        output.error_response_content_type)
			        .c_str());
		if (!output.error_response_content_encoding.empty())
			event.add_text("error_response_content_encoding",
			    sanitize_operation_detail(
			        output.error_response_content_encoding)
			        .c_str());
		event.add_text("session_id", task->session_id.c_str());
		event.add_number(
		    "completed_tool_calls", runtime_completed_tool_count(task));
	}
	append_runtime_operation_event(task, event);
}

} // namespace agent_detail
} // namespace action
