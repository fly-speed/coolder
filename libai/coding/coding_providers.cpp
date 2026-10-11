#include "coding_internal.h"
#include "../provider/ai_provider_store.h"
#include "../sdk/provider_model.h"
#include "../provider/output_token_limit.h"
#include <algorithm>
namespace webcool
{
namespace ai
{
namespace coding_detail
{
namespace
{
void record(acl::json_node &out, const provider_config_t &p)
{
	out.add_text("id", p.id.c_str())
	    .add_text("name", p.name.c_str())
	    .add_text("protocol", p.protocol.c_str())
	    .add_text("base_url", p.base_url.c_str())
	    .add_text("model", p.model.c_str())
	    .add_text("api_key_hint", p.api_key_hint.c_str())
	    .add_bool("enabled", p.enabled)
	    .add_bool("allow_file_content", p.allow_file_content)
	    .add_bool("is_default", p.is_default)
	    .add_text("last_test_status", p.last_test_status.c_str())
	    .add_number("last_test_at", p.last_test_at)
	    .add_number("last_test_http_status", p.last_test_http_status)
	    .add_number("last_test_latency_ms", p.last_test_latency_ms)
	    .add_text("last_test_error", p.last_test_error.c_str());
}
libai::message_t message(acl::json_node &n)
{
	libai::message_t m;
	const auto role = text(n, "role");
	if (role == "user")
		m.role = libai::message_t::role_t::user;
	else if (role == "assistant")
		m.role = libai::message_t::role_t::assistant;
	else if (role == "tool")
		m.role = libai::message_t::role_t::tool;
	else
		throw std::invalid_argument("invalid conversation role");
	m.text = text(n, "text");
	m.call_id = text(n, "call_id");
	m.reasoning = text(n, "reasoning");
	m.tool_error = boolean(n, "tool_error", false);
	auto *calls = array_value(n["calls"]);
	if (calls) {
		if (!calls->is_array())
			throw std::invalid_argument("calls must be an array");
		for (auto *c = calls->first_child(); c;
		     c = calls->next_child()) {
			if (!c->is_object() || m.calls.size() >= 100)
				throw std::invalid_argument(
				    "invalid tool calls");
			libai::tool_call_t call;
			call.id = text(*c, "id");
			call.name = text(*c, "name");
			call.arguments = text(*c, "arguments");
			m.calls.push_back(call);
		}
	}
	return m;
}
bool test_connection(provider_store_t &store, acl::json_node &a,
    acl::json_node &out, std::string &error)
{
	std::vector<provider_config_t> providers;
	if (!store.list(providers, error))
		return false;
	const auto id = text(a, "id");
	const provider_config_t *selected = nullptr;
	for (const auto &p : providers)
		if (p.id == id)
			selected = &p;
	if (!selected) {
		error = "provider is absent";
		return false;
	}
	std::string key;
	if (!store.reveal_api_key(*selected, key, error))
		return false;
	provider_test_result_t result;
	const bool ok =
	    provider_client_t::test_connection(*selected, key, result, error);
	std::fill(key.begin(), key.end(), '\0');
	std::string health_error;
	const bool saved =
	    store.record_test_result(selected->id, ok, result.http_status,
	        result.latency_ms, ok ? "" : error, health_error);
	if (!ok)
		return false;
	if (!saved) {
		error = health_error;
		return false;
	}
	out.add_number("http_status", result.http_status)
	    .add_number("latency_ms", result.latency_ms)
	    .add_text("endpoint", result.endpoint.c_str())
	    .add_bool("model_verified", result.model_verified)
	    .add_bool("responses_verified", result.responses_verified)
	    .add_bool("tools_verified", result.tools_verified);
	return true;
}
bool complete(provider_store_t &store, acl::json_node &a, acl::json_node &out,
    std::string &error)
{
	std::vector<provider_config_t> providers;
	if (!store.list(providers, error))
		return false;
	const auto id = text(a, "id");
	const provider_config_t *selected = nullptr;
	for (const auto &p : providers)
		if (p.id == id)
			selected = &p;
	if (!selected || !selected->enabled) {
		error = "provider is absent or disabled";
		return false;
	}
	std::string key;
	if (!store.reveal_api_key(*selected, key, error))
		return false;
	libai::model_request_t request;
	request.instructions = text(a, "instructions");
	if (request.instructions.empty())
		request.instructions = "You are a helpful assistant.";
	const auto timeout = number(a, "timeout_ms", 30000),
	           tokens = number(a, "max_output_tokens", 4096);
	if (timeout <= 0 || timeout > 300000 || tokens <= 0 || tokens > 1000000)
		throw std::invalid_argument("invalid model budgets");
	request.deadline = std::chrono::steady_clock::now() +
	    std::chrono::milliseconds(timeout);
	request.max_output_tokens = static_cast<size_t>(tokens);
	request.cancellation = std::make_shared<libai::cancellation_t>();
	auto *messages = array_value(a["messages"]);
	if (!messages || !messages->is_array())
		throw std::invalid_argument("messages must be an array");
	for (auto *n = messages->first_child(); n; n = messages->next_child()) {
		if (!n->is_object() || request.messages.size() >= 500)
			throw std::invalid_argument(
			    "invalid or excessive messages");
		request.messages.push_back(message(*n));
	}
	auto *tools = array_value(a["tools"]);
	if (tools) {
		if (!tools->is_array())
			throw std::invalid_argument("tools must be an array");
		for (auto *n = tools->first_child(); n;
		     n = tools->next_child()) {
			if (!n->is_object() || request.tools.size() >= 100)
				throw std::invalid_argument(
				    "invalid or excessive tools");
			libai::tool_spec_t spec;
			spec.name = text(*n, "name");
			spec.description = text(*n, "description");
			spec.parameters = text(*n, "parameters");
			request.tools.push_back(spec);
		}
	}
	libai::provider_model_t model(*selected, key);
	std::vector<completion_image_t> images;
	auto *input_images = array_value(a["images"]);
	if (input_images) {
		if (!input_images->is_array())
			throw std::invalid_argument("images must be an array");
		for (auto *n = input_images->first_child(); n;
		     n = input_images->next_child()) {
			if (images.size() >= 8)
				throw std::invalid_argument("too many images");
			completion_image_t image;
			image.name = text(*n, "name");
			image.mime_type = text(*n, "mime_type");
			const auto encoded = text(*n, "base64");
			if (encoded.size() > 12 * 1024 * 1024 ||
			    encoded.find_first_not_of(
			        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=") !=
			        std::string::npos)
				throw std::invalid_argument(
				    "invalid image base64");
			acl::string bytes;
			bytes.base64_decode(encoded.c_str());
			image.data.assign(bytes.c_str(), bytes.size());
			if (image.data.empty() ||
			    image.data.size() > 8 * 1024 * 1024)
				throw std::invalid_argument(
				    "invalid image size");
			images.push_back(image);
		}
	}
	model.set_images(images);
	const auto result = model.complete(request, libai::stream_sink_t());
	out.add_text("text", result.text.c_str())
	    .add_text("reasoning", result.reasoning.c_str())
	    .add_number("input_tokens", result.input_tokens)
	    .add_number("output_tokens", result.output_tokens)
	    .add_bool("retryable", result.retryable)
	    .add_number("cached_input_tokens",
	        model.last_completion().cached_input_tokens)
	    .add_bool("cache_usage_available",
	        model.last_completion().cache_usage_available)
	    .add_number("latency_ms", model.last_completion().latency_ms);
	auto &array = coding_detail::array(out, "calls");
	for (const auto &c : result.calls)
		array.add_child(false, true)
		    .add_text("id", c.id.c_str())
		    .add_text("name", c.name.c_str())
		    .add_text("arguments", c.arguments.c_str());
	error = result.error;
	const auto &native = model.last_completion();
	const auto effective = native.effective_max_output_tokens > 0 ?
	    native.effective_max_output_tokens :
	    tokens;
	out.add_number("effective_max_output_tokens", effective)
	    .add_number("reasoning_tokens", native.reasoning_tokens);
	annotate_output_token_limit(error, native.incomplete_reason, effective);
	return error.empty();
}
}
bool providers(const std::string &root, const std::string &op,
    acl::json_node &a, acl::json_node &out, std::string &error)
{
	provider_store_t store(root, root, "app");
	if (op == "providers.list") {
		std::vector<provider_config_t> entries;
		if (!store.list(entries, error))
			return false;
		auto &array = coding_detail::array(out, "providers");
		for (const auto &p : entries)
			record(array.add_child(false, true), p);
		return true;
	}
	if (op == "providers.remove")
		return store.remove(text(a, "id"), error);
	if (op == "providers.test")
		return test_connection(store, a, out, error);
	if (op == "providers.complete")
		return complete(store, a, out, error);
	if (op == "providers.save") {
		provider_input_t p;
		provider_config_t saved;
		p.id = text(a, "id");
		p.name = text(a, "name");
		p.protocol = text(a, "protocol");
		p.base_url = text(a, "base_url");
		p.model = text(a, "model");
		p.api_key = text(a, "api_key");
		p.enabled = boolean(a, "enabled", true);
		p.allow_file_content = boolean(a, "allow_file_content", false);
		p.is_default = boolean(a, "is_default", false);
		p.clear_api_key = boolean(a, "clear_api_key", false);
		if (!store.save(p, saved, error))
			return false;
		record(out, saved);
		return true;
	}
	return false;
}
}
}
}
