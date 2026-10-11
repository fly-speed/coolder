#include <libai_cpp/agent.h>
#include <cassert>
#include <stdexcept>
#include <future>
#include <thread>

using namespace libai::cpp;

// Verify typed tool arguments, Unicode, streaming and persistent conversation.
static void test_session_and_tools()
{
	int models = 0, tools = 0;
	std::string streamed;
	Agent agent([&](const ModelRequest &request, const StreamSink &sink) {
		ModelResponse response;
		if (++models == 1) {
			ToolCall call;
			call.id = "编号";
			call.name = "echo";
			call.arguments = "{\"text\":\"你好🌏\"}";
			response.calls.push_back(call);
		} else {
			if (models == 2)
				assert(
				    request.messages.back().text == "你好🌏");
			response.text = "你好🌏";
			if (sink)
				assert(sink(response.text));
		}
		response.input_tokens = 2;
		response.output_tokens = 1;
		return response;
	});
	ToolSpec spec;
	spec.name = "echo";
	spec.requires_approval = true;
	agent.add_tool(
	    spec, [&](const ToolCall &call, const ToolContext &context) {
		assert(call.id == "编号" &&
		    call.arguments.find("你好🌏") != std::string::npos);
		assert(!context.stopped());
		++tools;
		return ToolResult::success("你好🌏");
	});
	RunOptions options;
	options.authorize = [](const ToolSpec &, const ToolCall &) {
		return true;
	};
	options.on_text = [&](const std::string &delta) {
		streamed += delta;
		return true;
	};
	auto result = agent.run("开始", options);
	assert(
	    result.status == RunStatus::completed && result.text == "你好🌏");
	assert(
	    tools == 1 && result.tool_calls == 1 && result.output_tokens == 2);
	assert(streamed == "你好🌏");
	const auto history = agent.messages();
	agent.run("继续");
	assert(agent.messages().size() > history.size());
	agent.reset();
	assert(agent.messages().empty());
	agent.run("新的会话");
	assert(agent.messages().size() == 2);
}

// Missing permissions deny side effects; budgets and model errors stay explicit.
static void test_denial_and_errors()
{
	int turns = 0, executed = 0;
	Agent agent([&](const ModelRequest &request, const StreamSink &) {
		ModelResponse response;
		if (++turns == 1) {
			ToolCall call;
			call.id = "1";
			call.name = "write";
			response.calls.push_back(call);
		} else {
			assert(request.messages.back().tool_error);
			response.text = "denied";
		}
		return response;
	});
	ToolSpec spec;
	spec.name = "write";
	spec.requires_approval = true;
	agent.add_tool(spec, [&](const ToolCall &, const ToolContext &) {
		++executed;
		return ToolResult::success("written");
	});
	assert(agent.run("write").text == "denied" && executed == 0);
	RunOptions options;
	options.max_model_calls = 0;
	assert(
	    agent.run("budget", options).status == RunStatus::budget_exceeded);
	Agent broken(
	    [](const ModelRequest &, const StreamSink &) -> ModelResponse {
		throw std::runtime_error("model failed");
	});
	assert(broken.run("fail").status == RunStatus::callback_error);
	Agent failed([](const ModelRequest &, const StreamSink &) {
		ModelResponse response;
		response.error = "provider rejected request";
		return response;
	});
	assert(failed.run("fail").status == RunStatus::model_error);
	options.timeout = std::chrono::milliseconds(-1);
	try {
		agent.run("invalid", options);
		assert(false);
	} catch (const std::invalid_argument &) {
	}
	agent.reset();
}

// Simultaneous and reentrant state access fails; cancellation crosses threads.
static void test_concurrency()
{
	std::promise<void> started;
	Agent *owner = nullptr;
	Agent agent([&](const ModelRequest &request, const StreamSink &) {
		try {
			owner->reset();
			assert(false);
		} catch (const std::logic_error &) {
		}
		started.set_value();
		while (!request.cancellation->cancelled() &&
		    std::chrono::steady_clock::now() < request.deadline)
			std::this_thread::yield();
		return ModelResponse();
	});
	owner = &agent;
	RunResult result;
	RunOptions options;
	options.timeout = std::chrono::seconds(5);
	auto ready = started.get_future();
	std::thread worker([&] { result = agent.run("wait", options); });
	assert(ready.wait_for(std::chrono::seconds(5)) ==
	    std::future_status::ready);
	try {
		agent.messages();
		assert(false);
	} catch (const std::logic_error &) {
	}
	try {
		agent.run("parallel");
		assert(false);
	} catch (const std::logic_error &) {
	}
	agent.cancel();
	worker.join();
	assert(result.status == RunStatus::cancelled);
	agent.reset();
	assert(agent.messages().empty());
}

// All translated callbacks retain the public cancellation token and full transcript.
static void test_translation_boundary()
{
	auto token = std::make_shared<Cancellation>();
	RunOptions options;
	options.cancellation = token;
	std::vector<Event::kind_t> events;
	options.on_event = [&](const Event &event) {
		events.push_back(event.kind);
	};
	int calls = 0;
	Agent agent([&](const ModelRequest &request, const StreamSink &) {
		assert(request.cancellation == token);
		assert(request.instructions == "instructions");
		assert(request.max_output_tokens == 4096);
		ModelResponse response;
		response.reasoning = "private reasoning";
		if (++calls == 1) {
			assert(request.tools.size() == 1);
			assert(
			    request.tools[0].description == "translation test");
			ToolCall call;
			call.id = "call-id";
			call.name = "test";
			call.arguments = "{\"value\":42}";
			response.calls.push_back(call);
		} else {
			assert(request.messages[1].role ==
			    Message::role_t::assistant);
			assert(request.messages[1].reasoning ==
			    "private reasoning");
			assert(request.messages[1].calls[0].arguments ==
			    "{\"value\":42}");
			assert(request.messages.back().role ==
			    Message::role_t::tool);
			assert(request.messages.back().call_id == "call-id");
			assert(request.messages.back().tool_error);
			response.text = "translated";
		}
		return response;
	}, "instructions");
	ToolSpec spec;
	spec.name = "test";
	spec.description = "translation test";
	agent.add_tool(
	    spec, [&](const ToolCall &call, const ToolContext &context) {
		assert(context.cancellation == token && !context.stopped());
		assert(call.id == "call-id");
		return ToolResult::failure("tool failure");
	});
	assert(agent.run("run", options).text == "translated");
	assert(events.front() == Event::kind_t::model_start);
	assert(events.back() == Event::kind_t::finished);
	auto history = agent.messages();
	assert(history[1].calls[0].id == "call-id" && history[2].tool_error);
	token->cancel();
	assert(agent.run("cancel", options).status == RunStatus::cancelled);
}

int main()
{
	test_session_and_tools();
	test_denial_and_errors();
	test_concurrency();
	test_translation_boundary();
}
