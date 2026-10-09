#include "stdafx.h"
#include "coding_runtime.h"
#include "../provider/ai_provider_client_internal.h"
#include "../provider/provider_request_scheduler.h"
#include <openssl/evp.h>
namespace action
{
namespace agent_detail
{

static bool save_generated_image(
    const std::shared_ptr<agent_runtime_task_t> &task,
    const std::string &response, webcool::ai::completion_result_t &output,
    std::string &err)
{
	using namespace webcool::ai;
	using namespace webcool::ai::provider_detail;
	acl::json result(response.c_str());
	if (!result.finish()) {
		err = "invalid image provider JSON response";
		return false;
	}
	acl::json_node *data = result["data"];
	acl::json_node *first = first_array_item(data);
	const std::string encoded =
	    first ? node_text(object_child(first, "b64_json")) : "";
	if (encoded.empty() || encoded.size() % 4 != 0 ||
	    encoded.size() > 24 * 1024 * 1024) {
		err =
		    "image provider returned no valid b64_json image; configure a model supporting the Images API";
		return false;
	}
	const size_t padding = encoded.back() == '=' ?
	    (encoded[encoded.size() - 2] == '=' ? 2 : 1) :
	    0;
	for (size_t i = 0; i < encoded.size() - padding; ++i) {
		const char ch = encoded[i];
		if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
		    (ch >= '0' && ch <= '9') || ch == '+' || ch == '/')
			continue;
		err = "invalid generated image encoding";
		return false;
	}
	std::string bytes(encoded.size() / 4 * 3, '\0');
	const int decoded =
	    EVP_DecodeBlock(reinterpret_cast<unsigned char *>(&bytes[0]),
	        reinterpret_cast<const unsigned char *>(encoded.data()),
	        static_cast<int>(encoded.size()));
	if (decoded <= static_cast<int>(padding)) {
		err = "invalid generated image encoding";
		return false;
	}
	bytes.resize(decoded - padding);
	std::string mime;
	if (bytes.size() >= 24 &&
	    bytes.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0)
		mime = "image/png";
	else if (bytes.size() >= 4 &&
	    bytes.compare(0, 3, "\xff\xd8\xff", 3) == 0)
		mime = "image/jpeg";
	else if (bytes.size() >= 16 && bytes.compare(0, 4, "RIFF") == 0 &&
	    bytes.compare(8, 4, "WEBP") == 0)
		mime = "image/webp";
	else {
		err = "generated image must be PNG, JPEG or WebP";
		return false;
	}
	if (runtime_cancel_requested(task)) {
		err = "agent run cancelled";
		return false;
	}
	if (!assistant_session_store_t(task->user_root)
	         .save_image(task->id, task->conversation_id, mime, bytes, err))
		return false;
	output.text =
	    "![AI image](/api/v1/ai/assistant/images?id=" + task->id + ")";
	acl::json_node *usage = result["usage"];
	output.input_tokens =
	    usage ? json_number(object_child(usage, "input_tokens"), -1) : -1;
	output.output_tokens =
	    usage ? json_number(object_child(usage, "output_tokens"), -1) : -1;
	return true;
}

bool run_assistant_image_task(const std::shared_ptr<agent_runtime_task_t> &task,
    const webcool::ai::provider_config_t &provider, const std::string &api_key,
    webcool::ai::completion_result_t &output, std::string &err)
{
	using namespace webcool::ai;
	using namespace webcool::ai::provider_detail;
	output.input_tokens = output.output_tokens = -1;
	struct elapsed_guard_t {
		completion_result_t &result;
		std::chrono::steady_clock::time_point started;
		~elapsed_guard_t()
		{
			result.latency_ms = std::chrono::duration_cast<
			    std::chrono::milliseconds>(
			    std::chrono::steady_clock::now() - started)
			                        .count();
		}
	} elapsed_guard{ output, std::chrono::steady_clock::now() };
	if (!task->assistant_chat || runtime_cancel_requested(task)) {
		err = "agent run cancelled";
		return false;
	}
	acl::json chat(task->original_prompt.c_str());
	const std::string prompt =
	    chat.finish() ? json_text(chat["user_message"]) : "";
	if (prompt.empty() || prompt.size() > 32000) {
		err =
		    "image generation requires a text prompt (maximum 32000 bytes)";
		return false;
	}
	acl::json payload;
	acl::json_node &root = payload.create_node();
	root.add_text("model", provider.model.c_str());
	root.add_text("prompt", prompt.c_str());
	root.add_number("n", 1);
	if (task->image_size != "auto")
		root.add_text("size", task->image_size.c_str());
	// GPT Image always returns base64; compatible legacy services require an
	// explicit response_format to avoid short-lived remote image URLs.
	if (provider.model.compare(0, 9, "gpt-image") != 0) {
		root.add_text("response_format", "b64_json");
	}
	acl::string serialized;
	root.to_string(&serialized);
	std::string base = provider.base_url;
	while (!base.empty() && base.back() == '/')
		base.pop_back();
	const std::string endpoint = base +
	    ((base.size() >= 3 &&
	         base.compare(base.size() - 3, 3, "/v1") == 0) ?
	            "" :
	            "/v1") +
	    "/images/generations";
	parsed_url_t parsed;
	if (!parse_http_url(endpoint, parsed, err))
		return false;
	std::unique_ptr<acl::openssl_conf> ssl;
	if (parsed.use_ssl) {
		ssl.reset(new acl::openssl_conf(false));
		if (!configure_tls(*ssl, parsed.verify_host, err))
			return false;
	}
	acl::http_request request(parsed.address.c_str(),
	    provider_connect_timeout_seconds(), 300, true);
	if (ssl)
		request.set_ssl(ssl.get()).set_ssl_sni(
		    parsed.verify_host.c_str());
	acl::http_header &header = request.request_header();
	header.set_url(parsed.path.c_str())
	    .set_host(parsed.host.c_str())
	    .set_keep_alive(false)
	    .set_content_type("application/json");
	add_auth_headers(header, provider, api_key);
	std::shared_ptr<agent_runtime_task_t> cancel_context = task;
	provider_request_permit_t permit;
	if (!provider_request_scheduler_t::acquire(
	        provider_scheduler_key(provider, api_key), [](void *context) {
		return runtime_cancel_requested(
		    *static_cast<const std::shared_ptr<agent_runtime_task_t> *>(
		        context));
	}, &cancel_context, permit, err))
		return false;
	struct permit_guard_t {
		provider_request_permit_t &permit;
		completion_result_t &result;
		~permit_guard_t()
		{
			provider_request_scheduler_t::finish(permit,
			    result.http_status, result.http_status == 429, 0);
		}
	} permit_guard{ permit, output };
	const auto started = std::chrono::steady_clock::now();
	if (!request.post(serialized.c_str(), serialized.size())) {
		err = "image provider connection failed or timed out";
		return false;
	}
	output.http_status = request.http_status();
	const bool http_ok =
	    output.http_status >= 200 && output.http_status < 300;
	const size_t limit = http_ok ? 24 * 1024 * 1024 : 256 * 1024;
	std::string response;
	if (request.body_length() > static_cast<long long>(limit)) {
		err = "image provider response exceeds size limit";
		return false;
	}
	for (;;) {
		if (runtime_cancel_requested(task)) {
			err = "agent run cancelled";
			return false;
		}
		acl::string chunk;
		const int got = request.read_body(chunk, true);
		if (got < 0) {
			err = "cannot read image provider response";
			return false;
		}
		if (response.size() + chunk.size() > limit) {
			err = "image provider response exceeds size limit";
			return false;
		}
		response.append(chunk.c_str(), chunk.size());
		if (got == 0)
			break;
		if (!(std::chrono::steady_clock::now() - started >
		        std::chrono::seconds(300)))
			continue;
		err = "image generation timed out";
		return false;
	}
	output.latency_ms =
	    std::chrono::duration_cast<std::chrono::milliseconds>(
	        std::chrono::steady_clock::now() - started)
	        .count();
	if (http_ok)
		return save_generated_image(task, response, output, err);
	err = provider_client_t::describe_http_error(
	    output.http_status, response, "", "", "", "");
	return false;
}

} // namespace agent_detail
} // namespace action
