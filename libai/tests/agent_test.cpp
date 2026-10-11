#include <libai/agent.h>
#include <cassert>
#include <iostream>

using namespace libai;

static tool_call_t call(const std::string &id, const std::string &name = "test")
{
	tool_call_t result;
	result.id = id;
	result.name = name;
	result.arguments = "{\"nested\":{\"values\":[1,true]}}";
	return result;
}
static std::shared_ptr<model_t> model(function_model_t::function_t function)
{
	return std::make_shared<function_model_t>(function);
}
static tool_spec_t spec(bool approval = false)
{
	tool_spec_t result;
	result.name = "test";
	result.requires_approval = approval;
	return result;
}

// Verify ordered tools, permissions and complete cancellation transcripts.
static void test_tools()
{
	// Multiple ordered calls, arbitrary arguments, and subsequent user turns.
	int turns = 0, executed = 0;
	agent_t agent(model(
	    [&](const model_request_t &request, const stream_sink_t &sink) {
		model_response_t response;
		response.output_tokens = 2;
		if (turns++ == 0) {
			response.calls = { call("a"), call("b") };
			assert(request.tools.size() == 1);
		} else if (turns == 2) {
			assert(request.messages.size() == 4);
			assert(request.messages[2].call_id == "a");
			assert(request.messages[3].call_id == "b");
			assert(sink("done"));
			response.text = "done";
		} else {
			assert(request.messages.back().text == "follow up");
			response.text = "again";
		}
		return response;
	}));
	agent.add_tool(
	    spec(), [&](const tool_call_t &input, const tool_context_t &) {
		assert(input.arguments == call("x").arguments);
		++executed;
		return tool_result_t::success(std::to_string(executed));
	});
	agent_session_t session;
	run_options_t options;
	std::string streamed;
	options.on_text = [&](const std::string &delta) {
		streamed += delta;
		return true;
	};
	auto result = agent.run(session, "start", options);
	assert(
	    result.status == run_status_t::completed && result.text == "done");
	assert(result.model_calls == 2 && result.tool_calls == 2 &&
	    result.output_tokens == 4);
	assert(streamed == "done" && executed == 2);
	assert(agent.run(session, "follow up").text == "again");

	// Approval defaults to denial; failures are returned to the model as tools.
	auto one_call =
	    model([](const model_request_t &request, const stream_sink_t &) {
		model_response_t response;
		if (request.messages.size() == 1)
			response.calls = { call("a") };
		else {
			assert(request.messages.back().tool_error);
			response.text = "denied";
		}
		return response;
	});
	agent_t protected_agent(one_call);
	protected_agent.add_tool(
	    spec(true), [&](const tool_call_t &, const tool_context_t &) {
		++executed;
		return tool_result_t::success("should not execute");
	});
	agent_session_t denied;
	assert(protected_agent.run(denied, "start").status ==
	        run_status_t::completed &&
	    executed == 2);

	// No partial execution when a batch exceeds the tool budget or has bad IDs.
	for (bool duplicate : { false, true }) {
		agent_t invalid(
		    model([&](const model_request_t &, const stream_sink_t &) {
			model_response_t response;
			response.calls = { call("x"),
				call(duplicate ? "x" : "y") };
			return response;
		}));
		invalid.add_tool(
		    spec(), [&](const tool_call_t &, const tool_context_t &) {
			++executed;
			return tool_result_t::success("unexpected");
		});
		run_options_t limited;
		limited.max_tool_calls = 1;
		agent_session_t state;
		assert(invalid.run(state, "start", limited).status ==
		    (duplicate ? run_status_t::protocol_error :
		                 run_status_t::budget_exceeded));
		assert(executed == 2 && state.messages.size() == 1);
	}

	// tool_t exceptions, missing tools and oversized results close every exchange.
	agent_t failures(
	    model([](const model_request_t &request, const stream_sink_t &) {
		model_response_t response;
		if (request.messages.size() == 1)
			response.calls = { call("a"), call("b", "unknown"),
				call("c", "large") };
		else {
			assert(request.messages.size() == 5);
			for (std::size_t i = 2; i < 5; ++i)
				assert(request.messages[i].tool_error);
			assert(request.messages.back().text.size() == 8);
			response.text = "handled";
		}
		return response;
	}));
	failures.add_tool(spec(),
	    [](const tool_call_t &, const tool_context_t &) -> tool_result_t {
		throw std::runtime_error("failed");
	});
	auto large = spec();
	large.name = "large";
	failures.add_tool(
	    large, [](const tool_call_t &, const tool_context_t &) {
		return tool_result_t::success(std::string(100, 'x'));
	});
	agent_session_t failed;
	run_options_t bounded;
	bounded.max_tool_result_bytes = 8;
	assert(failures.run(failed, "start", bounded).text == "handled");

	// cancellation_t during a batch executes no later mutations but closes calls.
	agent_t cancellation(
	    model([](const model_request_t &, const stream_sink_t &) {
		model_response_t response;
		response.calls = { call("a"), call("b") };
		return response;
	}));
	cancellation.add_tool(
	    spec(), [](const tool_call_t &, const tool_context_t &context) {
		context.cancellation->cancel();
		return tool_result_t::success("performed");
	});
	agent_session_t cancelled;
	auto stopped = cancellation.run(cancelled, "start");
	assert(stopped.status == run_status_t::cancelled &&
	    stopped.tool_calls == 1);
	assert(cancelled.messages.size() == 4 &&
	    cancelled.messages.back().tool_error);

	bool duplicate_rejected = false;
	try {
		agent.add_tool(
		    spec(), [](const tool_call_t &, const tool_context_t &) {
			return tool_result_t();
		});
	} catch (const std::invalid_argument &) {
		duplicate_rejected = true;
	}
	assert(duplicate_rejected);
}

// Verify bounded retries, model failures, deadlines and stream cancellation.
static void test_control()
{
	// Retry only model errors; budgets apply to attempts and reported usage.
	int attempts = 0;
	agent_t retry(
	    model([&](const model_request_t &, const stream_sink_t &) {
		model_response_t response;
		if (++attempts == 1) {
			response.error = "transient";
			response.retryable = true;
		} else
			response.text = "recovered";
		return response;
	}));
	agent_session_t retried;
	assert(retry.run(retried, "start").model_calls == 2);
	attempts = 0;
	agent_session_t no_retry;
	run_options_t zero;
	zero.max_model_retries = 0;
	assert(retry.run(no_retry, "start", zero).status ==
	    run_status_t::model_error);
	agent_t token_limit(
	    model([](const model_request_t &, const stream_sink_t &) {
		model_response_t response;
		response.output_tokens = 2;
		response.calls = { call("x") };
		return response;
	}));
	agent_session_t tokens;
	run_options_t token_options;
	token_options.max_output_tokens = 1;
	assert(token_limit.run(tokens, "start", token_options).status ==
	    run_status_t::budget_exceeded);
	assert(tokens.messages.size() == 1);
	agent_t oversized(
	    model([](const model_request_t &, const stream_sink_t &) {
		model_response_t response;
		response.text = std::string(20, 'x');
		return response;
	}));
	agent_session_t oversized_state;
	run_options_t small;
	small.max_context_bytes = 24;
	assert(oversized.run(oversized_state, "start", small).status ==
	    run_status_t::budget_exceeded);

	// Pre-cancel, deadline, context, stream cancellation and callback exceptions.
	agent_t never(model([](const model_request_t &,
	                        const stream_sink_t &) -> model_response_t {
		assert(false);
		return model_response_t();
	}));
	agent_session_t empty;
	run_options_t pre;
	pre.cancellation = std::make_shared<cancellation_t>();
	pre.cancellation->cancel();
	assert(
	    never.run(empty, "start", pre).status == run_status_t::cancelled &&
	    empty.messages.empty());
	pre.cancellation.reset();
	pre.timeout = std::chrono::milliseconds(0);
	assert(never.run(empty, "start", pre).status ==
	    run_status_t::deadline_exceeded);
	pre.timeout = std::chrono::milliseconds(1000);
	pre.max_context_bytes = 1;
	assert(never.run(empty, "start", pre).status ==
	    run_status_t::budget_exceeded);
	agent_t stream(
	    model([](const model_request_t &, const stream_sink_t &sink) {
		assert(!sink("stop"));
		return model_response_t();
	}));
	agent_session_t streamed_state;
	run_options_t reject;
	reject.on_text = [](const std::string &) { return false; };
	assert(stream.run(streamed_state, "start", reject).status ==
	    run_status_t::cancelled);
	agent_t broken(model([](const model_request_t &,
	                         const stream_sink_t &) -> model_response_t {
		throw std::runtime_error("model exception");
	}));
	agent_session_t broken_state;
	assert(broken.run(broken_state, "start").status ==
	    run_status_t::callback_error);
}

int main()
{
	test_tools();
	test_control();
	std::cout << "portable agent tests passed\n";
}
