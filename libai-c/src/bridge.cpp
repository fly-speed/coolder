#include "bridge_internal.h"
#include <cstdlib>
#include <cstring>

/* Configuration changes fail immediately rather than deadlocking reentrant callbacks. */
static std::unique_lock<std::mutex> idle_lock(libai_agent *agent)
{
	if (!agent)
		throw std::invalid_argument("agent is null");
	std::unique_lock<std::mutex> lock(agent->mutex, std::try_to_lock);
	if (!lock.owns_lock())
		throw std::logic_error("agent is running");
	return lock;
}

/* Translate stable ABI limits and hooks into native SDK options. */
static libai::run_options_t run_options(
    const libai_options *limits, const libai_hooks *hooks)
{
	libai::run_options_t result;
	if (limits) {
		const auto maximum =
		    std::chrono::duration_cast<std::chrono::milliseconds>(
		        std::chrono::steady_clock::duration::max())
		        .count() /
		    2;
		if (limits->timeout_ms < 0 || limits->timeout_ms > maximum)
			throw std::invalid_argument("timeout is out of range");
		result.max_model_calls = limits->max_model_calls;
		result.max_tool_calls = limits->max_tool_calls;
		result.max_output_tokens = limits->max_output_tokens;
		result.max_tokens_per_call = limits->max_tokens_per_call;
		result.max_context_bytes = limits->max_context_bytes;
		result.max_tool_result_bytes = limits->max_tool_result_bytes;
		result.max_model_retries = limits->max_model_retries;
		result.timeout = std::chrono::milliseconds(limits->timeout_ms);
	}
	if (!hooks)
		return result;
	const auto copy = *hooks;
	if (copy.on_text)
		result.on_text = [copy](const std::string &text) {
			return copy.on_text(copy.user, text.c_str()) != 0;
		};
	if (copy.authorize)
		result.authorize = [copy](const libai::tool_spec_t &spec,
		                       const libai::tool_call_t &call) {
			return copy.authorize(copy.user, call.name.c_str(),
			           call.arguments.c_str(),
			           spec.requires_approval) != 0;
		};
	if (copy.on_event)
		result.on_event = [copy](const libai::agent_event_t &event) {
			const auto text = bridge_event_json(event);
			copy.on_event(copy.user, text.c_str());
		};
	return result;
}

extern "C" {
libai_agent *libai_agent_create(
    libai_model_fn model, void *user, const char *instructions)
{
	try {
		bridge_error("");
		if (!model)
			throw std::invalid_argument("model callback is null");
		std::unique_ptr<libai_agent> agent(new libai_agent);
		agent->model = std::make_shared<libai::function_model_t>(
		    [model, user](const libai::model_request_t &request,
		        const libai::stream_sink_t &sink) {
			libai::model_response_t output;
			libai_response builder;
			builder.value = &output;
			libai_invocation invocation;
			invocation.cancellation = request.cancellation;
			invocation.deadline = request.deadline;
			invocation.sink = sink;
			const auto json = bridge_request_json(request);
			model(user, json.c_str(), &builder, &invocation);
			if (builder.failed)
				throw std::runtime_error(
				    "model output allocation failed");
			return output;
		});
		agent->runtime.reset(new libai::agent_t(
		    agent->model, bridge_text(instructions)));
		return agent.release();
	} catch (const std::exception &error) {
		bridge_error(error.what());
	} catch (...) {
		bridge_error("native agent creation failed");
	}
	return NULL;
}
int libai_agent_destroy(libai_agent *agent)
{
	if (!agent)
		return 0;
	try {
		auto lock = idle_lock(agent);
		lock.unlock();
		delete agent;
		return 0;
	} catch (const std::exception &error) {
		bridge_error(error.what());
	} catch (...) {
		bridge_error("native destruction failed");
	}
	return -1;
}
int libai_agent_add_tool(libai_agent *agent, const char *name,
    const char *description, const char *schema, int approval,
    libai_tool_fn tool, void *user)
{
	try {
		auto lock = idle_lock(agent);
		if (!tool)
			throw std::invalid_argument("tool callback is null");
		libai::tool_spec_t spec;
		spec.name = bridge_text(name);
		spec.description = bridge_text(description);
		if (schema)
			spec.parameters = schema;
		spec.requires_approval = approval != 0;
		agent->runtime->add_tool(spec,
		    [tool, user](const libai::tool_call_t &call,
		        const libai::tool_context_t &context) {
			libai::tool_result_t output;
			libai_tool_result builder;
			builder.value = &output;
			libai_invocation invocation;
			invocation.cancellation = context.cancellation;
			invocation.deadline = context.deadline;
			tool(user, call.id.c_str(), call.name.c_str(),
			    call.arguments.c_str(), &builder, &invocation);
			if (builder.failed)
				throw std::runtime_error(
				    "tool output allocation failed");
			return output;
		});
		return 0;
	} catch (const std::exception &error) {
		bridge_error(error.what());
	} catch (...) {
		bridge_error("native tool registration failed");
	}
	return -1;
}
char *libai_agent_run_with_cancellation(libai_agent *agent, const char *input,
    const libai_options *limits, const libai_hooks *hooks,
    const libai_cancellation *cancellation)
{
	try {
		auto lock = idle_lock(agent);
		auto options = run_options(limits, hooks);
		options.cancellation = cancellation ?
		    cancellation->value :
		    std::make_shared<libai::cancellation_t>();
		std::atomic_store(&agent->cancellation, options.cancellation);
		const auto result = agent->runtime->run(
		    agent->session, bridge_text(input), options);
		const auto json = bridge_result_json(result);
		char *text = static_cast<char *>(std::malloc(json.size() + 1));
		if (!text)
			throw std::bad_alloc();
		std::memcpy(text, json.c_str(), json.size() + 1);
		return text;
	} catch (const std::exception &error) {
		bridge_error(error.what());
	} catch (...) {
		bridge_error("native run failed");
	}
	return NULL;
}
char *libai_agent_run(libai_agent *agent, const char *input,
    const libai_options *limits, const libai_hooks *hooks)
{
	return libai_agent_run_with_cancellation(
	    agent, input, limits, hooks, nullptr);
}
char *libai_agent_messages(libai_agent *agent)
{
	try {
		bridge_error("");
		auto lock = idle_lock(agent);
		return bridge_owned(
		    bridge_messages_json(agent->session.messages));
	} catch (const std::exception &error) {
		bridge_error(error.what());
	} catch (...) {
		bridge_error("session snapshot failed");
	}
	return nullptr;
}
void libai_agent_cancel(libai_agent *agent)
{
	if (!agent)
		return;
	auto cancellation = std::atomic_load(&agent->cancellation);
	if (cancellation)
		cancellation->cancel();
}
int libai_agent_reset(libai_agent *agent)
{
	try {
		auto lock = idle_lock(agent);
		agent->session.messages.clear();
		return 0;
	} catch (const std::exception &error) {
		bridge_error(error.what());
	} catch (...) {
		bridge_error("native session reset failed");
	}
	return -1;
}
}
