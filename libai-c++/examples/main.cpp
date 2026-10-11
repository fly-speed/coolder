#include <libai_cpp/agent.h>
#include <iostream>

int main()
{
	using namespace libai::cpp;
	// Replace this offline backend with the application's model transport.
	Agent app([](const ModelRequest &request, const StreamSink &) {
		ModelResponse response;
		if (request.messages.back().role == Message::role_t::user) {
			ToolCall call;
			call.id = "status_1";
			call.name = "app.status";
			response.calls.push_back(call);
		} else
			response.text = "Application status: " +
			    request.messages.back().text;
		return response;
	}, "Use app.status to report the application state.");
	ToolSpec status;
	status.name = "app.status";
	status.description = "Get the application's current state";
	app.add_tool(status, [](const ToolCall &, const ToolContext &) {
		return ToolResult::success("ready");
	});
	const auto result = app.run("Check application status");
	if (result.status != RunStatus::completed) {
		std::cerr << result.error << '\n';
		return 1;
	}
	std::cout << result.text << '\n';
}
