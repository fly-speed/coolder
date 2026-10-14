#include "bridge_internal.h"
#include <libai/sdk/governance.h>
#include <cstdlib>

/* No exception, allocator ownership or C++ object crosses the language boundary. */
static thread_local std::string last_error;
static thread_local bool error_allocation_failed = false;

std::string bridge_text(const char *text)
{
	return text ? text : "";
}

void bridge_error(const char *text) noexcept
{
	try {
		last_error = text;
		error_allocation_failed = false;
	} catch (...) {
		error_allocation_failed = true;
	}
}

extern "C" {
void libai_execution_result_set(libai_execution_result *r,const char *revision,const char *state,const char *error) {
    if(!r)return;
    try{r->revision=bridge_text(revision);r->state_json=bridge_text(state);r->error=bridge_text(error);}
    catch(...){r->failed=true;}
}
const char *libai_invocation_idempotency_key(const libai_invocation *invocation) {
    return invocation?invocation->idempotency_key.c_str():"";
}

char *libai_governance_call(const char *operation,const char *arguments)
{
    try {
        return bridge_owned(libai::governance_call(bridge_text(operation),
            arguments?bridge_text(arguments):"{}"));
    } catch (const std::exception &e) { bridge_error(e.what()); }
    catch (...) { bridge_error("governance operation failed"); }
    return nullptr;
}
unsigned libai_abi_version(void)
{
	return 1;
}
const char *libai_last_error(void)
{
	return error_allocation_failed ? "native error allocation failed" :
	                                 last_error.c_str();
}
void libai_string_free(char *text)
{
	std::free(text);
}
void libai_options_default(libai_options *options)
{
	if (!options)
		return;
	libai::run_options_t source;
	options->max_model_calls = source.max_model_calls;
	options->max_tool_calls = source.max_tool_calls;
	options->max_output_tokens = source.max_output_tokens;
	options->max_tokens_per_call = source.max_tokens_per_call;
	options->max_context_bytes = source.max_context_bytes;
	options->max_tool_result_bytes = source.max_tool_result_bytes;
	options->max_model_retries = source.max_model_retries;
	options->timeout_ms = source.timeout.count();
}
void libai_response_text(
    libai_response *response, const char *text, const char *reasoning)
{
	if (!response)
		return;
	try {
		response->value->text = bridge_text(text);
		response->value->reasoning = bridge_text(reasoning);
	} catch (...) {
		response->failed = true;
	}
}
void libai_response_call(libai_response *response, const char *id,
    const char *name, const char *arguments)
{
	if (!response)
		return;
	try {
		libai::tool_call_t call;
		call.id = bridge_text(id);
		call.name = bridge_text(name);
		call.arguments = arguments ? arguments : "{}";
		response->value->calls.push_back(call);
	} catch (...) {
		response->failed = true;
	}
}
void libai_response_usage(libai_response *response, size_t input, size_t output)
{
	if (!response)
		return;
	response->value->input_tokens = input;
	response->value->output_tokens = output;
}
void libai_response_error(
    libai_response *response, const char *text, int retryable)
{
	if (!response)
		return;
	try {
		response->value->error = bridge_text(text);
		response->value->retryable = retryable != 0;
	} catch (...) {
		response->failed = true;
	}
}
void libai_tool_result_set(
    libai_tool_result *result, const char *text, int error)
{
	if (!result)
		return;
	try {
		result->value->text = bridge_text(text);
		result->value->error = error != 0;
	} catch (...) {
		result->failed = true;
	}
}
int libai_invocation_stopped(const libai_invocation *invocation)
{
	return !invocation ||
	    (invocation->cancellation &&
	        invocation->cancellation->cancelled()) ||
	    std::chrono::steady_clock::now() >= invocation->deadline;
}
int libai_invocation_emit(libai_invocation *invocation, const char *text)
{
	if (libai_invocation_stopped(invocation))
		return 0;
	try {
		return invocation->sink && invocation->sink(bridge_text(text));
	} catch (...) {
		if (invocation->cancellation)
			invocation->cancellation->cancel();
		return 0;
	}
}
}
