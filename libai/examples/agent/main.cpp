#include <libai/agent.h>
#include <iostream>

int main()
{
	// Replace this offline callback with your model SDK or libai::provider_model_t.
	auto model = std::make_shared<libai::function_model_t>(
	    [](const libai::model_request_t &request,
	        const libai::stream_sink_t &) {
		libai::model_response_t response;
		if (request.messages.back().role ==
		    libai::message_t::role_t::user) {
			libai::tool_call_t call;
			call.id = "call_1";
			call.name = "app.status";
			response.calls.push_back(call);
		} else
			response.text = "Application status: " +
			    request.messages.back().text;
		return response;
	});
	libai::agent_t agent(
	    model, "Use app.status to report application status.");
	libai::tool_spec_t status;
	status.name = "app.status";
	status.description = "Get application status";
	agent.add_tool(status,
	    [](const libai::tool_call_t &, const libai::tool_context_t &) {
		return libai::tool_result_t::success("ready");
	});
	libai::agent_session_t session;
	auto result = agent.run(session, "Check application status");
	if (result.status != libai::run_status_t::completed) {
		std::cerr << result.error << '\n';
		return 1;
	}
	std::cout << result.text << '\n';
}
