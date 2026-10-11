#include <libai/sdk/workspace_tools.h>
#include <libai/common/file_ops.h>
#include <cassert>
#include <cstdio>
#include <iostream>
#ifndef _WIN32
#include <unistd.h>
#endif

static libai::message_t invoke(const std::string &root, const std::string &name,
    const std::string &arguments, bool authorize = false)
{
	auto model = std::make_shared<libai::function_model_t>(
	    [&](const libai::model_request_t &request,
	        const libai::stream_sink_t &) {
		libai::model_response_t response;
		if (request.messages.size() == 1) {
			libai::tool_call_t call;
			call.id = "test_call";
			call.name = name;
			call.arguments = arguments;
			response.calls.push_back(call);
		} else
			response.text = "finished";
		return response;
	});
	libai::agent_t agent(model);
	libai::add_workspace_tools(agent, root, true);
	libai::agent_session_t session;
	libai::run_options_t options;
	if (authorize)
		options.authorize = [](const libai::tool_spec_t &,
		                        const libai::tool_call_t &) {
			return true;
		};
	assert(agent.run(session, "test", options).status ==
	    libai::run_status_t::completed);
	assert(session.messages.size() == 4);
	return session.messages[2];
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	const std::string root = argv[1];
	assert(webcool::ai::file_ops::make_private_directory(root));
	const std::string file = root + "/fixture.cpp";
	std::remove(file.c_str());
	const std::string create =
	    R"({"path":"fixture.cpp","content":"int answer = 42;\n"})";
	assert(invoke(root, "workspace.create", create).tool_error);
	assert(!webcool::ai::file_ops::path_entry_exists(file));
	assert(!invoke(root, "workspace.create", create, true).tool_error);
	assert(invoke(root, "workspace.create", create, true).tool_error);
	auto read = invoke(root, "workspace.read", R"({"path":"fixture.cpp"})");
	assert(!read.tool_error);
	acl::json result(read.text.c_str());
	assert(result.finish());
	const auto digest =
	    webcool::ai::json_value::string_text(result["sha256"]);
	assert(digest.size() == 64);
	assert(!invoke(root, "workspace.list", R"({"path":""})").tool_error);
	auto matches =
	    invoke(root, "workspace.search", R"({"path":"","query":"answer"})");
	assert(!matches.tool_error &&
	    matches.text.find("fixture.cpp") != std::string::npos);
	assert(invoke(root, "workspace.read", R"({"path":"../outside.txt"})")
	           .tool_error);
	assert(invoke(root, "workspace.read", R"({"path":3})").tool_error);
	assert(invoke(root, "workspace.read", "{}").tool_error);
	assert(invoke(root, "workspace.replace",
	    R"({"path":"fixture.cpp","expected_sha256":"bad","content":"bad"})",
	    true)
	           .tool_error);
	const std::string replace =
	    "{\"path\":\"fixture.cpp\",\"expected_sha256\":\"" + digest +
	    "\",\"content\":\"int answer = 43;\\n\"}";
	assert(!invoke(root, "workspace.replace", replace, true).tool_error);
	assert(invoke(root, "workspace.replace", replace, true).tool_error);
	assert(invoke(root, "workspace.read", R"({"path":"fixture.cpp"})")
	           .text.find("43") != std::string::npos);
#ifndef _WIN32
	const std::string link = root + "/escape";
	std::remove(link.c_str());
	assert(symlink("..", link.c_str()) == 0);
	assert(
	    invoke(root, "workspace.read", R"({"path":"escape/outside.txt"})")
	        .tool_error);
	assert(invoke(root, "workspace.create",
	    R"({"path":"escape/escaped.txt","content":"bad"})", true)
	           .tool_error);
	std::remove(link.c_str());
#endif
	std::remove(file.c_str());
	std::cout << "SDK workspace tests passed\n";
}
