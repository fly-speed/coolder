#pragma once
#include <libai_cpp/agent.h>
#include <libai/agent.h>
namespace libai
{
namespace cpp
{
// Backend cancellation storage is never declared in a public header.
struct Cancellation::Impl {
	// Native handle shared with the private SDK runtime.
	std::shared_ptr<libai::cancellation_t> native{
		std::make_shared<libai::cancellation_t>()
	};
};
namespace detail
{
template <class Target, class Source, class Convert>
// Copy ordered value objects without depending on compatible object layouts.
std::vector<Target> convert_values(
    const std::vector<Source> &source, Convert convert)
{
	std::vector<Target> result;
	result.reserve(source.size());
	for (const auto &value : source)
		result.push_back(convert(value));
	return result;
}
// Translate a complete call into either independently declared value type.
template <class Target, class Source> Target call_value(const Source &source)
{
	Target target;
	target.id = source.id;
	target.name = source.name;
	target.arguments = source.arguments;
	return target;
}
// Translate a capability schema without retaining references to callbacks.
template <class Target, class Source> Target spec_value(const Source &source)
{
	Target target;
	target.name = source.name;
	target.description = source.description;
	target.parameters = source.parameters;
	target.requires_approval = source.requires_approval;
	return target;
}
// Preserve every conversation role explicitly, independent of enum values.
inline Message message_value(const libai::message_t &source)
{
	Message target;
	switch (source.role) {
	case libai::message_t::role_t::user:
		target.role = Message::role_t::user;
		break;
	case libai::message_t::role_t::assistant:
		target.role = Message::role_t::assistant;
		break;
	case libai::message_t::role_t::tool:
		target.role = Message::role_t::tool;
		break;
	}
	target.text = source.text;
	target.call_id = source.call_id;
	target.reasoning = source.reasoning;
	target.tool_error = source.tool_error;
	target.calls = convert_values<ToolCall>(
	    source.calls, call_value<ToolCall, libai::tool_call_t>);
	return target;
}
// Translate facade conversation values into the shared native SDK.
inline libai::message_t native_message(const Message &source)
{
	libai::message_t target;
	switch (source.role) {
	case Message::role_t::user:
		target.role = libai::message_t::role_t::user;
		break;
	case Message::role_t::assistant:
		target.role = libai::message_t::role_t::assistant;
		break;
	case Message::role_t::tool:
		target.role = libai::message_t::role_t::tool;
		break;
	}
	target.text = source.text;
	target.call_id = source.call_id;
	target.reasoning = source.reasoning;
	target.tool_error = source.tool_error;
	target.calls = convert_values<libai::tool_call_t>(
	    source.calls, call_value<libai::tool_call_t, ToolCall>);
	return target;
}
// Copy native transport results into independently declared facade values.
inline ModelResponse public_response(const libai::model_response_t &source)
{
	ModelResponse target;
	target.text = source.text;
	target.reasoning = source.reasoning;
	target.calls = convert_values<ToolCall>(
	    source.calls, call_value<ToolCall, libai::tool_call_t>);
	target.input_tokens = source.input_tokens;
	target.output_tokens = source.output_tokens;
	target.error = source.error;
	target.retryable = source.retryable;
	return target;
}
// Normalize transport output into backend values at the implementation boundary.
inline libai::model_response_t response_value(const ModelResponse &source)
{
	libai::model_response_t target;
	target.text = source.text;
	target.reasoning = source.reasoning;
	target.calls = convert_values<libai::tool_call_t>(
	    source.calls, call_value<libai::tool_call_t, ToolCall>);
	target.input_tokens = source.input_tokens;
	target.output_tokens = source.output_tokens;
	target.error = source.error;
	target.retryable = source.retryable;
	return target;
}
// Translate lifecycle phases explicitly rather than assuming equal enum layouts.
inline Event event_value(const libai::agent_event_t &source)
{
	Event::kind_t kind = Event::kind_t::finished;
	switch (source.kind) {
	case libai::agent_event_t::kind_t::model_start:
		kind = Event::kind_t::model_start;
		break;
	case libai::agent_event_t::kind_t::model_end:
		kind = Event::kind_t::model_end;
		break;
	case libai::agent_event_t::kind_t::tool_start:
		kind = Event::kind_t::tool_start;
		break;
	case libai::agent_event_t::kind_t::tool_end:
		kind = Event::kind_t::tool_end;
		break;
	case libai::agent_event_t::kind_t::finished:
		kind = Event::kind_t::finished;
		break;
	}
	Event target(kind);
	target.tool_name = source.tool_name;
	target.call_id = source.call_id;
	target.error = source.error;
	return target;
}
// Preserve terminal state and accounting independently of backend enum numbering.
inline RunResult result_value(const libai::run_result_t &source)
{
	RunResult target;
	switch (source.status) {
	case libai::run_status_t::completed:
		target.status = RunStatus::completed;
		break;
	case libai::run_status_t::cancelled:
		target.status = RunStatus::cancelled;
		break;
	case libai::run_status_t::deadline_exceeded:
		target.status = RunStatus::deadline_exceeded;
		break;
	case libai::run_status_t::budget_exceeded:
		target.status = RunStatus::budget_exceeded;
		break;
	case libai::run_status_t::model_error:
		target.status = RunStatus::model_error;
		break;
	case libai::run_status_t::protocol_error:
		target.status = RunStatus::protocol_error;
		break;
	case libai::run_status_t::callback_error:
		target.status = RunStatus::callback_error;
		break;
	}
	target.text = source.text;
	target.error = source.error;
	target.model_calls = source.model_calls;
	target.tool_calls = source.tool_calls;
	target.input_tokens = source.input_tokens;
	target.output_tokens = source.output_tokens;
	return target;
}
}

}
}
