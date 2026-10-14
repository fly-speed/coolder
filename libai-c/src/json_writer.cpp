#include "bridge_internal.h"
#include <sstream>

/* Escape JSON strings while retaining valid UTF-8 supplied by the application. */
static std::string quoted(const std::string &text)
{
	const char *hex = "0123456789abcdef";
	std::string result = "\"";
	for (unsigned char c : text) {
		if (c == '"' || c == '\\') {
			result += '\\';
			result += c;
		} else if (c < 32) {
			result += "\\u00";
			result += hex[c >> 4];
			result += hex[c & 15];
		} else
			result += c;
	}
	return result + '"';
}

/* Append a field whose value is textual rather than an embedded JSON object. */
static void field(std::ostream &out, const char *name, const std::string &value)
{
	out << quoted(name) << ':' << quoted(value);
}

/* Arguments and schemas stay strings, preserving arbitrary application JSON. */
static void calls(
    std::ostream &out, const std::vector<libai::tool_call_t> &items)
{
	out << '[';
	bool first = true;
	for (const auto &call : items) {
		if (!first)
			out << ',';
		first = false;
		out << '{';
		field(out, "id", call.id);
		out << ',';
		field(out, "name", call.name);
		out << ',';
		field(out, "arguments", call.arguments);
		out << '}';
	}
	out << ']';
}

std::string bridge_messages_json(const std::vector<libai::message_t> &messages)
{
	std::ostringstream out;
	out << '[';
	bool first = true;
	for (const auto &message : messages) {
		if (!first)
			out << ',';
		first = false;
		out << '{';
		const char *role =
		    message.role == libai::message_t::role_t::user ?
		    "user" :
		    message.role == libai::message_t::role_t::assistant ?
		    "assistant" :
		    "tool";
		field(out, "role", role);
		out << ',';
		field(out, "text", message.text);
		out << ',';
		field(out, "call_id", message.call_id);
		out << ',';
		field(out, "reasoning", message.reasoning);
		out << ",\"tool_error\":"
		    << (message.tool_error ? "true" : "false") << ",\"calls\":";
		calls(out, message.calls);
		out << '}';
	}
	out << ']';
	return out.str();
}

std::string bridge_request_json(const libai::model_request_t &request)
{
	std::ostringstream out;
	out << '{';
	field(out, "instructions", request.instructions);
    if(!request.output_schema.empty()) {
        out << ','; field(out,"output_schema",request.output_schema);
        out << ",\"output_repair\":" << (request.output_repair?"true":"false");
    }
	out << ",\"model_profile\":" << request.profile.json;
	out << ",\"max_output_tokens\":" << request.max_output_tokens
	    << ",\"messages\":" << bridge_messages_json(request.messages);
	out << ",\"tools\":[";
	bool first = true;
	for (const auto &tool : request.tools) {
		if (!first)
			out << ',';
		first = false;
		out << '{';
		field(out, "name", tool.name);
		out << ',';
		field(out, "description", tool.description);
		out << ',';
		field(out, "parameters", tool.parameters);
		out << ",\"requires_approval\":"
		    << (tool.requires_approval ? "true" : "false") << '}';
	}
	out << "]}";
	return out.str();
}

std::string bridge_result_json(const libai::run_result_t &result)
{
	const char *names[] = { "completed", "cancelled", "deadline_exceeded",
		"budget_exceeded", "model_error", "protocol_error",
		"callback_error", "recovery_required" };
	std::ostringstream out;
	out << '{';
	field(out, "status", names[static_cast<unsigned>(result.status)]);
	out << ',';
	field(out, "text", result.text);
	out << ',';
	field(out, "error", result.error);
	out << ",\"model_calls\":" << result.model_calls
	    << ",\"tool_calls\":" << result.tool_calls;
	out << ",\"input_tokens\":" << result.input_tokens
	    << ",\"output_tokens\":" << result.output_tokens;
    out << ",\"output_validated\":" << (result.output_validated?"true":"false")
        << ",\"output_repairs\":" << result.output_repairs;
	out << ",\"task_contract\":{\"version\":1,\"loop_finished\":"
        << (result.task_contract.loop_finished ? "true" : "false") << ',';
    field(out, "loop_outcome", result.task_contract.loop_outcome);
    const auto &c = result.task_contract;
    out << ',';
    field(out, "artifact_revision", c.artifact_revision);
    out << ",\"requirements\":{\"generation\":"
        << (c.require_generation ? "true" : "false") << ",\"build\":"
        << (c.require_build ? "true" : "false") << ",\"tests\":"
        << (c.require_tests ? "true" : "false") << ",\"review\":"
        << (c.require_review ? "true" : "false") << '}';
    const char *stages[] = { "generation", "build", "tests", "review" };
    const libai::task_evidence_t *evidence[] = { &c.generation, &c.build, &c.tests, &c.review };
    for (unsigned i = 0; i < 4; ++i) {
        out << ',' << quoted(stages[i]) << ":{";
        field(out, "state", evidence[i]->state);
        out << ','; field(out, "revision", evidence[i]->revision);
        out << ','; field(out, "authority", evidence[i]->authority);
        out << ','; field(out, "reference", evidence[i]->reference);
        out << '}';
    }
    out << ",\"accepted\":" << (c.accepted() ? "true" : "false") << "}}";
	return out.str();
}

std::string bridge_event_json(const libai::agent_event_t &event)
{
	const char *names[] = { "model_start", "model_end", "tool_start",
		"tool_end", "finished", "tool_intent_committed", "tool_result_committed", "tool_result_reused" };
	std::ostringstream out;
	out << '{';
	field(out, "kind", names[static_cast<unsigned>(event.kind)]);
	out << ',';
	field(out, "tool_name", event.tool_name);
	out << ',';
	field(out, "call_id", event.call_id);
	out << ",\"error\":" << (event.error ? "true" : "false");
    out << ',';field(out,"execution_scope",event.execution_scope);
    out << ',';field(out,"idempotency_key",event.idempotency_key);
    out << ',';field(out,"journal_revision",event.journal_revision);
    out << ',';field(out,"effect_state",event.effect_state);
    out << ",\"attempts\":" << event.attempts << '}';
	return out.str();
}

std::string bridge_execution_snapshot_json(const libai::execution_snapshot_t &snapshot)
{
    return "{\"revision\":" + quoted(snapshot.revision) + ",\"state_json\":" + quoted(snapshot.state_json) + "}";
}
