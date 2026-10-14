#include <libai/sdk/provider_model.h>
#include <libai/common/json_value.h>
#include <libai/provider/ai_provider_client_internal.h>
#include <cassert>
#include <iostream>

using namespace libai::ai;
using namespace libai::ai::json_value;

static std::size_t count(acl::json_node *array)
{
	std::size_t result = 0;
	for (auto *node = array->first_child(); node;
	     node = array->next_child())
		++result;
	return result;
}

int main()
{
	// Explicit compatible API prefixes must not receive an extra /v1.
	for (const std::string path : { "", "/", "/v1", "/v1/", "/api/paas/v4/",
	         "/api/coding/paas/v4", "/gateway/custom/", "/compatible-mode/v1" }) {
		provider_config_t provider;
		provider.base_url = "https://example.invalid" + path;
		const std::string prefix = "https://example.invalid" +
		    (path.empty() || path == "/" ? "/v1" : (path.back() == '/' ? path.substr(0, path.size() - 1) : path));
		for (const std::string protocol : { "openai_chat", "openai_compatible", "openai_responses" }) {
			provider.protocol = protocol;
			const std::string route = protocol == "openai_responses" ? "/responses" : "/chat/completions";
			assert(provider_detail::test_url(provider) == prefix + "/models");
			assert(provider_detail::completion_url(provider, false) == prefix + route);
			assert(provider_detail::completion_url(provider, true) == prefix + route);
		}
		assert(provider_detail::responses_item_url(provider, "id/a", false) == prefix + "/responses/id%2Fa");
		assert(provider_detail::responses_item_url(provider, "id/a", true) == prefix + "/responses/id%2Fa/cancel");
		assert(provider_detail::responses_compact_url(provider) == prefix + "/responses/compact");
	}

	// Protocol completion must not depend on HTTP EOF or gateway keep-alive.
	for (const std::string protocol : { "openai_chat", "openai_responses",
	         "anthropic_messages", "ollama" }) {
		provider_config_t provider;
		provider.protocol = protocol;
		provider_detail::stream_diagnostics_t diagnostics;
		completion_result_t result;
		std::vector<provider_detail::streamed_tool_call_t> calls;
		std::string delta, reasoning, error;
		const std::string line = protocol == "ollama" ? R"({"done":true})" :
		    protocol == "anthropic_messages" ? R"(data: {"type":"message_stop"})" :
		    protocol == "openai_responses" ? R"(data: {"type":"response.completed"})" :
		    "data: [DONE]";
		assert(provider_detail::parse_stream_line(provider, line, result,
		    calls, delta, reasoning, diagnostics, error));
		assert(diagnostics.completed);
	}

	libai::model_request_t source;
	source.instructions = "application instructions";
	libai::tool_spec_t spec;
	spec.name = "app.search";
	spec.description = "Search records";
	spec.parameters =
	    R"({"type":"object","properties":{"limit":{"type":"integer"},"filters":{"type":"object"}},"required":["limit"]})";
	source.tools.push_back(spec);
	libai::message_t user;
	user.text = "search";
	source.messages.push_back(user);
	libai::message_t assistant;
	assistant.role = libai::message_t::role_t::assistant;
	assistant.text = "searching";
	libai::tool_call_t call;
	call.name = "app.search";
	call.id = "call_1";
	call.arguments =
	    R"({"limit":3,"filters":{"tags":["a","b"],"enabled":true}})";
	assistant.calls.push_back(call);
	call.id = "call_2";
	assistant.calls.push_back(call);
	source.messages.push_back(assistant);
	libai::message_t output;
	output.role = libai::message_t::role_t::tool;
	output.call_id = "call_1";
	output.text = "first";
	source.messages.push_back(output);
	output.call_id = "call_2";
	output.text = "second";
	source.messages.push_back(output);
	libai::message_t final;
	final.role = libai::message_t::role_t::assistant;
	final.text = "finished";
	source.messages.push_back(final);
	user.text = "follow up";
	source.messages.push_back(user);
	auto request = libai::provider_model_t::request(source);
	assert(request.user_prompt == "follow up");
	assert(request.messages.size() == source.messages.size());

	for (const std::string protocol : { "openai_chat", "openai_responses",
	         "anthropic_messages", "gemini_native", "ollama" }) {
		provider_config_t provider;
		provider.protocol = protocol;
		provider.model = "test";
		provider.base_url = "https://example.invalid";
		std::string payload;
		assert(provider_client_t::serialize_completion_request(
		    provider, request, false, payload));
		acl::json json(payload.c_str());
		assert(json.finish());
		auto *messages =
		    array_value(json[protocol == "gemini_native" ? "contents" :
		            protocol == "openai_responses"       ? "input" :
		                                                   "messages"]);
		assert(messages && count(messages) >= 5);
		auto *last = messages->first_child();
		for (auto *next = messages->next_child(); next;
		     next = messages->next_child())
			last = next;
		assert(string_text(object_child(last, "role")) == "user");
		if (protocol == "anthropic_messages" ||
		    protocol == "gemini_native") {
			const char *parts_field =
			    protocol == "gemini_native" ? "parts" : "content";
			auto *first_user = messages->first_child();
			(void)first_user;
			auto *model = messages->next_child();
			auto *parts =
			    array_value(object_child(model, parts_field));
			assert(count(parts) == 3); // text + two calls
			auto *text = parts->first_child();
			(void)text;
			auto *use = parts->next_child();
			auto *arguments = protocol == "gemini_native" ?
			    object_child(
			        object_child(use, "functionCall"), "args") :
			    object_child(use, "input");
			assert(number(object_child(arguments, "limit")) == 3);
			auto *results = messages->next_child();
			assert(count(array_value(
			           object_child(results, parts_field))) == 2);
			assert(count(messages) ==
			    5); // both results in the same user message
		}
		assert(payload.find("integer") != std::string::npos);
		if (protocol == "gemini_native")
			assert(payload.find("parametersJsonSchema") !=
			    std::string::npos);
		assert(payload.find("follow up") != std::string::npos);
		assert(payload.find("tags") != std::string::npos);
	}

	provider_config_t provider;
	provider.protocol = "openai_chat";
	completion_result_t parsed;
	std::string error;
	const std::string body =
	    R"({"choices":[{"message":{"content":"","tool_calls":[{"id":"call_1","type":"function","function":{"name":"app_search","arguments":"{\"limit\":3,\"filters\":{\"tags\":[\"a\",\"b\"],\"enabled\":true}}"}}]}}]})";
	assert(provider_client_t::parse_completion_response(
	    provider, body, parsed, error));
	assert(parsed.tool_calls.size() == 1 &&
	    parsed.tool_calls[0].name == "app.search");
	acl::json arguments(parsed.tool_calls[0].arguments_json.c_str());
	assert(arguments.finish());
	assert(number(arguments["limit"]) == 3);
	assert(boolean(object_child(arguments["filters"], "enabled")));
	assert(count(array_value(object_child(arguments["filters"], "tags"))) ==
	    2);

	const std::string stream = "data: " + body.substr(0, 1) +
	    R"("choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1","function":{"name":"app_search","arguments":"{\"limit\":3}"}}]}}]})" +
	    "\n\ndata: [DONE]\n\n";
	completion_result_t streamed;
	assert(provider_client_t::parse_stream_response(
	    provider, stream, streamed, error));
	assert(streamed.tool_calls.size() == 1 &&
	    !streamed.tool_calls[0].arguments_json.empty());
	request.tools[0].parameters_json = "broken";
	std::string payload;
	assert(!provider_client_t::serialize_completion_request(
	    provider, request, false, payload));
	bool collision = false;
	spec.name = "app_search";
	source.tools.push_back(spec);
	try {
		libai::provider_model_t::request(source);
	} catch (const std::invalid_argument &) {
		collision = true;
	}
	assert(collision);
	libai::model_request_t vision_source;
	libai::message_t user_image;
	user_image.role = libai::message_t::role_t::user;
	user_image.text = "Describe this image";
	vision_source.messages.push_back(user_image);
	vision_source.max_output_tokens = 64;
	auto vision = libai::provider_model_t::request(vision_source);
	completion_image_t image;
	image.name = "fixture.png";
	image.mime_type = "image/png";
	image.data = "PNG fixture";
	vision.images.push_back(image);
	for (const auto &protocol : { "openai_chat", "openai_responses",
	         "anthropic_messages", "gemini_native" }) {
		provider.protocol = protocol;
		std::string serialized;
		assert(provider_client_t::serialize_completion_request(
		    provider, vision, false, serialized));
		assert(serialized.find("UE5HIGZpeHR1cmU=") != std::string::npos);
		assert(serialized.find("Describe this image") != std::string::npos);
		assert(serialized.find("image/png") != std::string::npos);
	}
	std::cout << "SDK provider transcript tests passed\n";
}
