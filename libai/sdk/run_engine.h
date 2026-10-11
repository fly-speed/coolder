#pragma once
// Internal synchronous loop, split into bounded request/execution phases.
#include "agent_types.h"

namespace libai
{
namespace sdk_detail
{

// One run owns its counters and cancellation boundary; no global state.
class run_engine_t {
public:
	// Borrow immutable configuration and the caller-owned mutable session.
	run_engine_t(model_t &model, const std::string &instructions,
	    const std::map<std::string, tool_t> &tools,
	    agent_session_t &session, const run_options_t &options)
	        : model_(model)
	        , instructions_(instructions)
	        , tools_(tools)
	        , session_(session)
	        , options_(options)
	{
		context_.cancellation = options.cancellation ?
		    options.cancellation :
		    std::make_shared<cancellation_t>();
		context_.deadline =
		    std::chrono::steady_clock::now() + options.timeout;
	}

	// Finish through a single event path, including application exceptions.
	run_result_t execute(const std::string &input)
	{
		try {
			if (stopped())
				return finish();
			if (!options_.max_tokens_per_call ||
			    !options_.max_tool_result_bytes) {
				fail(run_status_t::budget_exceeded,
				    "invalid zero byte/token limit");
				return finish();
			}
			message_t user;
			user.text = input;
			session_.messages.push_back(user);
			while (step()) {
			}
		} catch (const std::exception &exception) {
			fail(run_status_t::callback_error, exception.what());
		} catch (...) {
			fail(run_status_t::callback_error,
			    "model/event callback failed");
		}
		return finish();
	}

private:
	// Record a terminal result and provide an early-return condition.
	bool fail(run_status_t status, const std::string &error)
	{
		result_.status = status;
		result_.error = error;
		return false;
	}
	// Observe cancellation before deadlines and before each execution phase.
	bool stopped()
	{
		if (!context_.stopped())
			return false;
		fail(context_.cancellation->cancelled() ?
		        run_status_t::cancelled :
		        run_status_t::deadline_exceeded,
		    "run stopped");
		return true;
	}
	// Notify observers on the run thread; phase callers handle exceptions.
	void emit(const agent_event_t &event)
	{
		if (options_.on_event)
			options_.on_event(event);
	}
	// Always return a result even if a terminal observer fails.
	run_result_t finish()
	{
		agent_event_t event(agent_event_t::kind_t::finished);
		event.error = result_.status != run_status_t::completed;
		try {
			emit(event);
		} catch (const std::exception &exception) {
			fail(run_status_t::callback_error, exception.what());
		} catch (...) {
			fail(run_status_t::callback_error,
			    "finished event callback failed");
		}
		return result_;
	}
	// Bound reported counters even for a defective custom model backend.
	static std::size_t add_usage(std::size_t total, std::size_t value)
	{
		const auto maximum = std::numeric_limits<std::size_t>::max();
		return value > maximum - total ? maximum : total + value;
	}
	// Count opaque argument bytes without parsing application-owned JSON.
	static std::size_t call_bytes(const tool_call_t &call)
	{
		return call.id.size() + call.name.size() +
		    call.arguments.size();
	}
	// Include reasoning replay and call identifiers in the context budget.
	static std::size_t message_bytes(const message_t &message)
	{
		std::size_t bytes = message.text.size() +
		    message.call_id.size() + message.reasoning.size();
		for (const auto &call : message.calls)
			bytes += call_bytes(call);
		return bytes;
	}
	// Snapshot the typed transcript and advertise the current tool catalog.
	std::size_t make_request(model_request_t &request)
	{
		request.instructions = instructions_;
		request.messages = session_.messages;
		request.cancellation = context_.cancellation;
		request.deadline = context_.deadline;
		request.max_output_tokens =
		    std::min(options_.max_tokens_per_call,
		        options_.max_output_tokens - result_.output_tokens);
		std::size_t bytes = instructions_.size();
		for (const auto &message : session_.messages)
			bytes += message_bytes(message);
		for (const auto &entry : tools_) {
			const auto &spec = entry.second.spec;
			request.tools.push_back(spec);
			bytes += spec.name.size() + spec.description.size() +
			    spec.parameters.size();
		}
		return bytes;
	}
	// Stream cancellation propagates to the model and later tool phases.
	bool text_delta(const std::string &delta)
	{
		if (context_.stopped())
			return false;
		if (options_.on_text && !options_.on_text(delta)) {
			context_.cancellation->cancel();
			return false;
		}
		return true;
	}
	// Count every logical attempt, including recoverable model failures.
	model_response_t call_model(const model_request_t &request)
	{
		emit(agent_event_t(agent_event_t::kind_t::model_start));
		++result_.model_calls;
		auto response =
		    model_.complete(request, [this](const std::string &delta) {
			return text_delta(delta);
		});
		result_.input_tokens =
		    add_usage(result_.input_tokens, response.input_tokens);
		result_.output_tokens =
		    add_usage(result_.output_tokens, response.output_tokens);
		emit(agent_event_t(agent_event_t::kind_t::model_end));
		return response;
	}
	// Validate the complete batch before any application tool has side effects.
	bool validate_response(
	    const model_response_t &response, std::size_t request_bytes)
	{
		if (result_.output_tokens > options_.max_output_tokens)
			return fail(run_status_t::budget_exceeded,
			    "provider exceeded token budget");
		std::size_t bytes =
		    response.text.size() + response.reasoning.size();
		for (const auto &call : response.calls) {
			bytes += call_bytes(call);
			if (call.id.empty() || call.name.empty() ||
			    !call_ids_.insert(call.id).second)
				return fail(run_status_t::protocol_error,
				    "empty or duplicate tool call identifier");
		}
		if (bytes > options_.max_context_bytes - request_bytes)
			return fail(run_status_t::budget_exceeded,
			    "model response exceeds context byte budget");
		if (response.calls.size() >
		    options_.max_tool_calls - result_.tool_calls)
			return fail(run_status_t::budget_exceeded,
			    "tool batch exceeds remaining budget");
		return true;
	}
	// Advance one model turn; false means a terminal state has been recorded.
	bool step()
	{
		if (stopped())
			return false;
		if (result_.model_calls >= options_.max_model_calls ||
		    result_.output_tokens >= options_.max_output_tokens)
			return fail(run_status_t::budget_exceeded,
			    "model/token budget exhausted");
		model_request_t request;
		const auto bytes = make_request(request);
		if (bytes > options_.max_context_bytes)
			return fail(run_status_t::budget_exceeded,
			    "context byte budget exhausted");
		auto response = call_model(request);
		if (stopped())
			return false;
		if (!response.error.empty()) {
			if (response.retryable &&
			    retries_++ < options_.max_model_retries)
				return true;
			return fail(run_status_t::model_error, response.error);
		}
		retries_ = 0;
		if (!validate_response(response, bytes))
			return false;
		message_t assistant;
		assistant.role = message_t::role_t::assistant;
		assistant.text = response.text;
		assistant.reasoning = response.reasoning;
		assistant.calls = response.calls;
		session_.messages.push_back(assistant);
		if (response.calls.empty()) {
			result_.text = response.text;
			return fail(run_status_t::completed, "");
		}
		// Ordered execution preserves dependencies between arbitrary mutations.
		for (const auto &call : response.calls)
			append_result(call, execute_tool(call));
		return true;
	}
	// Authorization may cancel a run, so check the boundary again afterward.
	tool_result_t dispatch_tool(const tool_call_t &call)
	{
		if (context_.stopped())
			return tool_result_t::failure(
			    "run stopped; tool skipped");
		const auto found = tools_.find(call.name);
		if (found == tools_.end())
			return tool_result_t::failure("unknown tool");
		const auto &tool = found->second;
		const bool allowed = options_.authorize ?
		    options_.authorize(tool.spec, call) :
		    !tool.spec.requires_approval;
		if (!allowed)
			return tool_result_t::failure(
			    "tool authorization denied");
		if (context_.stopped())
			return tool_result_t::failure(
			    "run stopped; tool skipped");
		++result_.tool_calls;
		return tool.execute(call, context_);
	}
	// Convert tool/authorization/start-observer exceptions into paired results.
	tool_result_t execute_tool(const tool_call_t &call)
	{
		try {
			agent_event_t event(agent_event_t::kind_t::tool_start);
			event.tool_name = call.name;
			event.call_id = call.id;
			emit(event);
			return dispatch_tool(call);
		} catch (const std::exception &exception) {
			return tool_result_t::failure(exception.what());
		} catch (...) {
			return tool_result_t::failure("tool callback failed");
		}
	}
	// Preserve UTF-8 boundaries and close every exchange, including skipped calls.
	void append_result(const tool_call_t &call, tool_result_t output)
	{
		if (output.text.size() > options_.max_tool_result_bytes) {
			std::size_t end = options_.max_tool_result_bytes;
			while (end &&
			    (static_cast<unsigned char>(output.text[end]) &
			        0xc0) == 0x80)
				--end;
			output.text.resize(end);
			output.error = true;
		}
		message_t message;
		message.role = message_t::role_t::tool;
		message.call_id = call.id;
		message.text = output.text;
		message.tool_error = output.error;
		session_.messages.push_back(message);
		agent_event_t event(agent_event_t::kind_t::tool_end);
		event.tool_name = call.name;
		event.call_id = call.id;
		event.error = output.error;
		// A presentation observer cannot prevent the remaining results pairing.
		try {
			emit(event);
		} catch (...) {
		}
	}

	// Application model and immutable configuration are borrowed for this run.
	model_t &model_;
	// Stable instructions copied into each provider request.
	const std::string &instructions_;
	// Registered application capabilities and their handlers.
	const std::map<std::string, tool_t> &tools_;
	// Caller-owned transcript retains partial side effects on cancellation.
	agent_session_t &session_;
	// Observer, authorization and budget settings for the current run.
	const run_options_t &options_;
	// Cooperative cancellation and deadline exposed to tool handlers.
	tool_context_t context_;
	// Final status and counters accumulated across attempts.
	run_result_t result_;
	// Reject repeated call IDs before executing a new batch.
	std::set<std::string> call_ids_;
	// Consecutive recoverable model failures in the current logical turn.
	std::size_t retries_ = 0;
};
} // namespace sdk_detail
} // namespace libai
