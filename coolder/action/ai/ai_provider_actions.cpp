#include "stdafx.h"
#include "ai_provider_actions.h"
#include "action/actions.h"
#include "action/action_util.h"
#include "libai/agent/agent_registry.h"
#include "libai/common/ai_error_log.h"
#include "libai/provider/ai_provider_client.h"
#include "libai/provider/ai_provider_store.h"

#include <string>
#include <vector>

namespace action {
namespace {

// Keep the browser response unchanged while recording the exact HTTP action
// failure. safe_error_for_log() bounds and flattens the message; API keys are
// never passed as an error string by this file.
#undef json_error

void ai_provider_json_error(response_t& res, int status, const char* message,
	bool keep_alive, const char* file, int line, const char* function) {
	webcool::ai::ai_log_error("http.ai.provider", "response",
		message ? message : "unspecified provider action error");
	action::json_error_at(res, status, message, keep_alive, file, line, function);
}

#define json_error(res, status, message, keep_alive)                                   \
	ai_provider_json_error(                                                            \
		res, status, message, keep_alive, __FILE__, __LINE__, __FUNCTION__)

std::string json_text(acl::json_node* node) {
	if (node == NULL) {
		return "";
	}
	const char* value = node->get_string();
	if (value == NULL) {
		value = node->get_text();
	}
	return value ? value : "";
}

bool json_bool(acl::json_node* node, bool fallback) {
	if (node == NULL) {
		return fallback;
	}
	const std::string value = json_text(node);
	if (value == "true" || value == "1") {
		return true;
	}
	if (value == "false" || value == "0") {
		return false;
	}
	return fallback;
}

bool authenticated_store(request_t& req, response_t& res, std::string& username,
	bool& admin, std::string& user_root, webcool::ai::provider_store_t*& store,
	bool require_admin) {
	const std::string upload_root = runtime_upload_dir_get();
	if (!auth_current_user(req, upload_root, username, admin)) {
		auth_send_required(req, res);
		return false;
	}
	if (require_admin && !admin) {
		json_error(res, 403, "admin permission required", req.isKeepAlive());
		return false;
	}
	std::string provider_username;
	std::string err;
	if (!auth_administrator_upload_dir(
			upload_root, provider_username, user_root, err)) {
		json_error(res, 500, err.c_str(), req.isKeepAlive());
		return false;
	}
	store =
		new webcool::ai::provider_store_t(upload_root, user_root, provider_username);
	return true;
}

void add_provider_json(acl::json_node& node, const webcool::ai::provider_config_t& item,
	bool include_key_hint = true) {
	node.add_text("id", item.id.c_str());
	node.add_text("name", item.name.c_str());
	node.add_text("protocol", item.protocol.c_str());
	node.add_text("base_url", item.base_url.c_str());
	node.add_text("model", item.model.c_str());
	node.add_bool("enabled", item.enabled);
	node.add_bool("allow_file_content", item.allow_file_content);
	node.add_bool("is_default", item.is_default);
	node.add_bool("responses_store", item.responses_store);
	node.add_bool("responses_background", item.responses_background);
	node.add_bool("responses_compact", item.responses_compact);
	node.add_bool("responses_strict_tools", item.responses_strict_tools);
	node.add_text(
		"responses_min_reasoning_effort", item.responses_min_reasoning_effort.c_str());
	node.add_text(
		"responses_reasoning_summary", item.responses_reasoning_summary.c_str());
	node.add_text("responses_text_verbosity", item.responses_text_verbosity.c_str());
	node.add_text("responses_service_tier", item.responses_service_tier.c_str());
	node.add_text("responses_cache_ttl", item.responses_cache_ttl.c_str());
	node.add_bool("qwen_session_cache", item.qwen_session_cache);
	node.add_text("openai_organization", item.openai_organization.c_str());
	node.add_text("openai_project", item.openai_project.c_str());
	node.add_bool("has_api_key", !item.api_key_ciphertext.empty());
	node.add_text("last_test_status",
		item.last_test_status.empty() ? "never" : item.last_test_status.c_str());
	node.add_number("last_test_at", item.last_test_at);
	node.add_number("last_test_http_status", item.last_test_http_status);
	node.add_number("last_test_latency_ms", item.last_test_latency_ms);
	if (include_key_hint && !item.api_key_hint.empty()) {
		node.add_text("api_key_hint", item.api_key_hint.c_str());
	}
	if (!item.last_test_error.empty()) {
		node.add_text("last_test_error", item.last_test_error.c_str());
	}
}

bool parse_provider_input(
	request_t& req, webcool::ai::provider_input_t& input, std::string& err) {
	acl::json* body = req.getJson(128 * 1024);
	if (body == NULL) {
		err = "invalid JSON body";
		return false;
	}
	input.id = json_text((*body)["id"]);
	input.name = json_text((*body)["name"]);
	input.protocol = json_text((*body)["protocol"]);
	input.base_url = json_text((*body)["base_url"]);
	input.model = json_text((*body)["model"]);
	input.api_key = json_text((*body)["api_key"]);
	input.enabled = json_bool((*body)["enabled"], true);
	input.allow_file_content = json_bool((*body)["allow_file_content"], false);
	input.is_default = json_bool((*body)["is_default"], false);
	input.clear_api_key = json_bool((*body)["clear_api_key"], false);
	input.responses_store = json_bool((*body)["responses_store"], true);
	input.responses_background = json_bool((*body)["responses_background"], false);
	input.responses_compact = json_bool((*body)["responses_compact"], true);
	input.responses_strict_tools = json_bool((*body)["responses_strict_tools"], true);
	input.responses_min_reasoning_effort =
		json_text((*body)["responses_min_reasoning_effort"]);
	input.responses_reasoning_summary =
		json_text((*body)["responses_reasoning_summary"]);
	input.responses_text_verbosity = json_text((*body)["responses_text_verbosity"]);
	input.responses_service_tier = json_text((*body)["responses_service_tier"]);
	input.responses_cache_ttl = json_text((*body)["responses_cache_ttl"]);
	input.qwen_session_cache = json_bool((*body)["qwen_session_cache"], false);
	input.openai_organization = json_text((*body)["openai_organization"]);
	input.openai_project = json_text((*body)["openai_project"]);
	if (input.responses_min_reasoning_effort.empty()) {
		input.responses_min_reasoning_effort = "auto";
	}
	if (input.responses_reasoning_summary.empty()) {
		input.responses_reasoning_summary = "auto";
	}
	if (input.responses_text_verbosity.empty()) {
		input.responses_text_verbosity = "medium";
	}
	if (input.responses_service_tier.empty()) {
		input.responses_service_tier = "auto";
	}
	if (input.responses_cache_ttl.empty()) {
		input.responses_cache_ttl = "30m";
	}
	return true;
}

} // namespace

bool AiProviderListAction::run(request_t& req, response_t& res) {
	std::string username;
	std::string user_root;
	bool admin = false;
	webcool::ai::provider_store_t* raw_store = NULL;
	if (!authenticated_store(req, res, username, admin, user_root, raw_store, false)) {
		return true;
	}
	std::unique_ptr<webcool::ai::provider_store_t> store(raw_store);
	std::vector<webcool::ai::provider_config_t> providers;
	std::string err;
	if (!store->list(providers, err)) {
		json_error(res, 500, err.c_str(), req.isKeepAlive());
		return true;
	}
	acl::json json;
	acl::json_node& root = json.create_node();
	root.add_bool("ok", true);
	acl::json_node& items = json.create_array();
	root.add_child("providers", items);
	for (size_t i = 0; i < providers.size(); ++i) {
		if (!admin && !providers[i].enabled) {
			continue;
		}
		acl::json_node& item = items.add_child(false, true);
		add_provider_json(item, providers[i], admin);
	}
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiProviderSaveAction::run(request_t& req, response_t& res) {
	std::string username;
	std::string user_root;
	bool admin = false;
	webcool::ai::provider_store_t* raw_store = NULL;
	if (!authenticated_store(req, res, username, admin, user_root, raw_store, true)) {
		return true;
	}
	std::unique_ptr<webcool::ai::provider_store_t> store(raw_store);
	webcool::ai::provider_input_t input;
	std::string err;
	if (!parse_provider_input(req, input, err)) {
		json_error(res, 400, err.c_str(), req.isKeepAlive());
		return true;
	}
	if (!webcool::ai::provider_store_t::validate_input(input, false, err)) {
		json_error(res, 400, err.c_str(), req.isKeepAlive());
		return true;
	}
	webcool::ai::provider_config_t saved;
	if (!store->save(input, saved, err)) {
		const int status = err == "AI provider not found" ? 404 : 500;
		json_error(res, status, err.c_str(), req.isKeepAlive());
		return true;
	}
	acl::json json;
	acl::json_node& root = json.create_node();
	root.add_bool("ok", true);
	acl::json_node& item = json.create_node();
	add_provider_json(item, saved);
	root.add_child("provider", item);
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiProviderDeleteAction::run(request_t& req, response_t& res) {
	std::string username;
	std::string user_root;
	bool admin = false;
	webcool::ai::provider_store_t* raw_store = NULL;
	if (!authenticated_store(req, res, username, admin, user_root, raw_store, true)) {
		return true;
	}
	std::unique_ptr<webcool::ai::provider_store_t> store(raw_store);
	std::string id = req.getParameter("id") ? req.getParameter("id") : "";
	if (id.empty() && req.getContentLength() > 0) {
		acl::json* body = req.getJson(32 * 1024);
		if (body != NULL) {
			id = json_text((*body)["id"]);
		}
	}
	std::string err;
	if (!store->remove(id, err)) {
		const int status = err == "AI provider not found" ? 404 : 400;
		json_error(res, status, err.c_str(), req.isKeepAlive());
		return true;
	}
	acl::json json;
	acl::json_node& root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("id", id.c_str());
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiProviderTestAction::run(request_t& req, response_t& res) {
	std::string username;
	std::string user_root;
	bool admin = false;
	webcool::ai::provider_store_t* raw_store = NULL;
	if (!authenticated_store(req, res, username, admin, user_root, raw_store, true)) {
		return true;
	}
	std::unique_ptr<webcool::ai::provider_store_t> store(raw_store);
	acl::json* body = req.getJson(32 * 1024);
	const std::string id = body ? json_text((*body)["id"]) : "";
	std::vector<webcool::ai::provider_config_t> providers;
	std::string err;
	if (!store->list(providers, err)) {
		json_error(res, 500, err.c_str(), req.isKeepAlive());
		return true;
	}
	const webcool::ai::provider_config_t* provider = NULL;
	for (size_t i = 0; i < providers.size(); ++i) {
		if (providers[i].id == id) {
			provider = &providers[i];
			break;
		}
	}
	if (provider == NULL) {
		json_error(res, 404, "AI provider not found", req.isKeepAlive());
		return true;
	}
	std::string api_key;
	if (!store->reveal_api_key(*provider, api_key, err)) {
		json_error(res, 500, err.c_str(), req.isKeepAlive());
		return true;
	}
	webcool::ai::provider_test_result_t result;
	result.http_status = 0;
	result.latency_ms = 0;
	webcool::ai::provider_usage_probe_t usage;
	std::string usage_err;
	const bool usage_ok = webcool::ai::provider_client_t::probe_usage_limits(
		*provider, api_key, usage, usage_err);
	bool ok = false;
	if (usage.supported && usage_ok && usage.checked) {
		// The authenticated account endpoint has already proved connectivity and
		// key validity, so do not spend a second request on /models.
		ok = true;
		result.http_status = usage.http_status;
		result.latency_ms = usage.latency_ms;
		result.endpoint = usage.endpoint;
	} else if (usage.supported && !usage_ok && usage.http_status != 0
		&& usage.http_status != 404 && usage.http_status != 405) {
		// Authentication, quota and rate-limit errors from the account endpoint
		// are authoritative and more useful than a second generic probe.
		err = usage_err;
		result.http_status = usage.http_status;
		result.latency_ms = usage.latency_ms;
		result.endpoint = usage.endpoint;
	} else {
		if (usage.supported && !usage_ok) {
			webcool::ai::ai_log_error(
				"http.ai.provider", "usage-probe-fallback", usage_err);
		}
		ok = webcool::ai::provider_client_t::test_connection(
			*provider, api_key, result, err);
	}
	std::fill(api_key.begin(), api_key.end(), '\0');
	// Record both successful and failed probes. This metadata is useful after a
	// restart and is stored separately from credentials and model responses.
	std::string health_err;
	if (!store->record_test_result(provider->id, ok, result.http_status,
			result.latency_ms, ok ? "" : err, health_err)) {
		if (ok) {
			json_error(res, 500, health_err.c_str(), req.isKeepAlive());
			return true;
		}
		// Preserve the actual connection failure for the browser. The store has
		// already emitted logger_error for this secondary persistence failure.
	}
	if (!ok) {
		json_error(res, 502, err.c_str(), req.isKeepAlive());
		return true;
	}
	acl::json json;
	acl::json_node& root = json.create_node();
	root.add_bool("ok", true);
	root.add_number("http_status", result.http_status);
	root.add_number("latency_ms", result.latency_ms);
	root.add_text("endpoint", result.endpoint.c_str());
	root.add_bool("model_verified", result.model_verified);
	root.add_bool("responses_verified", result.responses_verified);
	root.add_bool("tools_verified", result.tools_verified);
	acl::json_node& usage_json = json.create_node();
	root.add_child("usage_probe", usage_json);
	usage_json.add_bool("supported", usage.supported);
	usage_json.add_bool("checked", usage.checked);
	usage_json.add_bool("ok", usage_ok);
	usage_json.add_bool("balance_known", usage.balance_known);
	usage_json.add_bool("can_start", usage.can_start);
	if (usage.balance_known) {
		usage_json.add_double("available_balance", usage.available_balance);
		usage_json.add_text("currency", usage.currency.c_str());
	}
	usage_json.add_text("request_limit", usage.request_limit.c_str());
	usage_json.add_text("request_remaining", usage.request_remaining.c_str());
	usage_json.add_text("request_reset", usage.request_reset.c_str());
	usage_json.add_text("token_limit", usage.token_limit.c_str());
	usage_json.add_text("token_remaining", usage.token_remaining.c_str());
	usage_json.add_text("token_reset", usage.token_reset.c_str());
	usage_json.add_text("retry_after", usage.retry_after.c_str());
	usage_json.add_text("message", (usage_ok ? usage.message : usage_err).c_str());
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiAgentTypesAction::run(request_t& req, response_t& res) {
	std::string username;
	bool admin = false;
	if (!auth_current_user(req, runtime_upload_dir_get(), username, admin)) {
		auth_send_required(req, res);
		return true;
	}
	acl::json json;
	acl::json_node& root = json.create_node();
	root.add_bool("ok", true);
	acl::json_node& agents = json.create_array();
	root.add_child("agents", agents);
	const std::vector<webcool::ai::agent_definition_t>& definitions =
		webcool::ai::agent_registry_t::instance().list();
	for (size_t i = 0; i < definitions.size(); ++i) {
		const webcool::ai::agent_definition_t& definition = definitions[i];
		acl::json_node& item = agents.add_child(false, true);
		item.add_text("id", definition.id.c_str());
		item.add_text("version", definition.version.c_str());
		item.add_text("name", definition.name.c_str());
		item.add_text("description", definition.description.c_str());
		item.add_bool("enabled", definition.enabled);
		acl::json_node& protocols = json.create_array();
		item.add_child("provider_protocols", protocols);
		for (size_t j = 0; j < definition.provider_protocols.size(); ++j) {
			// ACL has a dedicated scalar-array constructor. add_text(NULL, ...)
			// treats NULL as an object tag and dereferences it while serializing.
			protocols.add_child(
				json.create_array_text(definition.provider_protocols[j].c_str()));
		}
		acl::json_node& tools = json.create_array();
		item.add_child("tools", tools);
		for (size_t j = 0; j < definition.tools.size(); ++j) {
			acl::json_node& tool = tools.add_child(false, true);
			tool.add_text("name", definition.tools[j].name.c_str());
			tool.add_text("description", definition.tools[j].description.c_str());
			tool.add_bool("mutating", definition.tools[j].mutating);
			tool.add_bool("model_enabled", definition.tools[j].model_enabled);
			tool.add_bool(
				"requires_file_content", definition.tools[j].requires_file_content);
			tool.add_text("authorization", definition.tools[j].authorization.c_str());
		}
	}
	return sendJson(res, 200, root, req.isKeepAlive());
}

} // namespace action

#undef json_error
