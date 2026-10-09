#include "stdafx.h"
#include "../common/json_value.h"
#include "ai_provider_client_internal.h"
#include "../common/ai_error_log.h"
#include "provider_output_limit_cache.h"
#include "../common/webcool_mutex.h"

#include <algorithm>
#include <ctime>
#include <map>
#include <mutex>

namespace webcool
{
namespace ai
{
namespace provider_detail
{

namespace
{
webcool::mutex g_provider_output_limit_mutex;
std::map<std::string, long long> g_provider_output_limits;
} // namespace

std::string provider_output_limit_key(const provider_config_t &provider)
{
	return provider.output_limit_cache_directory + "\n" + provider.id +
	    "\n" + provider.protocol + "\n" + provider.base_url + "\n" +
	    provider.model + "\n" + provider.openai_organization + "\n" +
	    provider.openai_project;
}

long long effective_provider_output_limit(
    const provider_config_t &provider, long long requested)
{
	std::lock_guard<webcool::mutex> guard(g_provider_output_limit_mutex);
	const std::string key = provider_output_limit_key(provider);
	auto found = g_provider_output_limits.find(key);
	if (!(found == g_provider_output_limits.end()))
		return found->second > 0 ? std::min(requested, found->second) :
		                           requested;
	const long long saved = load_output_limit_cache(
	    output_limit_cache_path(provider.output_limit_cache_directory, key),
	    std::time(NULL));
	found =
	    g_provider_output_limits.insert(std::make_pair(key, saved)).first;

	return found->second > 0 ? std::min(requested, found->second) :
	                           requested;
}

void remember_provider_output_limit(
    const provider_config_t &provider, long long limit)
{
	std::lock_guard<webcool::mutex> guard(g_provider_output_limit_mutex);
	long long &saved =
	    g_provider_output_limits[provider_output_limit_key(provider)];
	if (saved == 0 || limit < saved)
		saved = limit;
	if (!save_output_limit_cache(
	        output_limit_cache_path(provider.output_limit_cache_directory,
	            provider_output_limit_key(provider)),
	        saved, std::time(NULL)))
		ai_log_error("provider.client", "save-output-limit",
		    "Cannot persist learned output limit");
}

bool ends_with(const std::string &value, const std::string &suffix)
{
	return value.size() >= suffix.size() &&
	    value.compare(
	        value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string lowercase_ascii(std::string value)
{
	for (size_t i = 0; i < value.size(); ++i) {
		if (!(value[i] >= 'A' && value[i] <= 'Z'))
			continue;
		value[i] = static_cast<char>(value[i] - 'A' + 'a');
	}
	return value;
}

bool kimi_model_is(const provider_config_t &provider, const char *prefix)
{
	const std::string model = lowercase_ascii(provider.model);
	const std::string expected(prefix == NULL ? "" : prefix);
	return !expected.empty() &&
	    model.compare(0, expected.size(), expected) == 0;
}

bool kimi_model_always_thinks(const provider_config_t &provider)
{
	return kimi_model_is(provider, "kimi-k2.7-code") ||
	    kimi_model_is(provider, "kimi-k3");
}

bool model_name_contains(const provider_config_t &provider, const char *needle)
{
	return lowercase_ascii(provider.model).find(needle ? needle : "") !=
	    std::string::npos;
}

bool deepseek_responses_endpoint(const provider_config_t &provider)
{
	if (provider.protocol != "openai_responses")
		return false;
	if (model_name_contains(provider, "deepseek") &&
	    lowercase_ascii(provider.model).compare(0, 8, "deepseek") == 0) {
		return true;
	}
	const std::string base = lowercase_ascii(provider.base_url);
	// Match the official host (with an optional path such as /v1), but avoid
	// classifying unrelated hosts whose query/path merely mentions DeepSeek.
	return base.find("://api.deepseek.com") != std::string::npos ||
	    base.compare(0, strlen("api.deepseek.com"), "api.deepseek.com") ==
	    0;
}

bool kimi_responses_endpoint(const provider_config_t &provider)
{
	if (provider.protocol != "openai_responses")
		return false;
	if (kimi_model_is(provider, "kimi-"))
		return true;
	// Also recognize official endpoints when the model is a deployment alias.
	std::string host = lowercase_ascii(provider.base_url);
	const size_t scheme = host.find("://");
	if (scheme != std::string::npos)
		host.erase(0, scheme + 3);
	host = host.substr(0, host.find_first_of(":/?#"));
	return host == "api.kimi.com" || host == "api.moonshot.cn" ||
	    host == "api.moonshot.ai";
}

bool qwen_responses_endpoint(const provider_config_t &provider)
{
	if (provider.protocol != "openai_responses")
		return false;
	const std::string model = lowercase_ascii(provider.model);
	// Alibaba's compatible endpoint also hosts third-party models. The actual
	// model family always wins over the gateway/vendor selected in the UI.
	if (model.compare(0, 8, "deepseek") == 0 ||
	    model.compare(0, 4, "kimi") == 0)
		return false;
	if (model.compare(0, 4, "qwen") == 0)
		return true;
	// Official Qwen endpoints are also recognized for deployment aliases. The
	// compatible-mode path prevents unrelated Alibaba APIs from being classified.
	const std::string base = lowercase_ascii(provider.base_url);
	if (base.find("/compatible-mode/v1") == std::string::npos)
		return false;
	std::string host = base;
	const size_t scheme = host.find("://");
	if (scheme != std::string::npos)
		host.erase(0, scheme + 3);
	host = host.substr(0, host.find_first_of(":/?#"));
	const std::string maas_suffix = ".maas.aliyuncs.com";
	return host == "dashscope.aliyuncs.com" ||
	    host == "dashscope-intl.aliyuncs.com" ||
	    (host.size() > maas_suffix.size() &&
	        host.compare(host.size() - maas_suffix.size(),
	            maas_suffix.size(), maas_suffix) == 0);
}

std::string responses_reasoning_effort(
    const provider_config_t &provider, const std::string &requested)
{
	if (requested.empty())
		return requested;
	if (requested != "none")
		return requested;
	// Codex generations before GPT-5.6 expose low as their least expensive valid
	// effort. Sending none makes recovery/protocol-repair requests fail with 400.
	// A saved explicit minimum also covers private deployment aliases whose model
	// family cannot be inferred from the public slug.
	if (provider.responses_min_reasoning_effort == "low" ||
	    model_name_contains(provider, "gpt-5.3-codex") ||
	    model_name_contains(provider, "gpt-5.2-codex") ||
	    model_name_contains(provider, "gpt-5.1-codex") ||
	    (model_name_contains(provider, "gpt-5-codex") &&
	        !model_name_contains(provider, "gpt-5.6")))
		return "low";
	if (provider.responses_min_reasoning_effort == "none")
		return "none";
	if (qwen_responses_endpoint(provider))
		return "none";
	if (!model_name_contains(provider, "deepseek"))
		return model_name_contains(provider, "gpt-5.6") ? "none" : "";
	return "none";
	// Unknown official/compatible deployments are safer when the optional field
	// is omitted: the model applies its supported default instead of rejecting the
	// entire recovery request.
	return model_name_contains(provider, "gpt-5.6") ? "none" : "";
}

bool is_openai_reasoning_protocol(const provider_config_t &provider)
{
	// Do not infer capabilities from the configured model name. Compatible
	// gateways commonly expose deployment aliases that contain no vendor/model
	// family (for example, "coding-prod"). The caller invokes this helper only
	// after the stream has positively shown a reasoning-only, length-truncated
	// response, which is stronger evidence than any model-name convention.
	return provider.protocol == "openai_responses" ||
	    provider.protocol == "openai_chat" ||
	    provider.protocol == "openai_compatible";
}

bool reasoning_budget_exhausted(const std::string &err)
{
	// Older compatible streams reported a prose diagnostic, while native
	// Responses endpoints terminate with incomplete_details.reason. Treat both
	// wire representations as the same recoverable output-budget condition.
	return err.find("used the entire output-token budget for reasoning") !=
	    std::string::npos ||
	    err.find("response incomplete: max_output_tokens") !=
	    std::string::npos;
}

std::string trim_slashes(std::string value)
{
	while (!value.empty() && value[value.size() - 1] == '/')
		value.resize(value.size() - 1);
	return value;
}

std::string test_url(const provider_config_t &provider)
{
	const std::string base = trim_slashes(provider.base_url);
	if (provider.protocol == "ollama") {
		return ends_with(base, "/api") ? base + "/tags" :
		                                 base + "/api/tags";
	}
	if (!(provider.protocol == "gemini_native"))
		return ends_with(base, "/v1") ? base + "/models" :
		                                base + "/v1/models";
	return (ends_with(base, "/v1beta") || ends_with(base, "/v1")) ?
	    base + "/models?pageSize=1" :
	    base + "/v1beta/models?pageSize=1";
}

std::string kimi_balance_url(const provider_config_t &provider)
{
	const std::string base = trim_slashes(provider.base_url);
	// Standard Kimi/Moonshot endpoints end in /v1. Kimi Code Plan endpoints can
	// include /coding/v1; the account API lives at the host-level /v1 path.
	const size_t scheme = base.find("://");
	const size_t path = scheme == std::string::npos ?
	    std::string::npos :
	    base.find('/', scheme + 3);
	const std::string origin =
	    path == std::string::npos ? base : base.substr(0, path);
	return origin + "/v1/users/me/balance";
}

std::string percent_encode_path(const std::string &value)
{
	static const char *digits = "0123456789ABCDEF";
	std::string out;
	for (size_t i = 0; i < value.size(); ++i) {
		const unsigned char ch = static_cast<unsigned char>(value[i]);
		if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
		    (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
		    ch == '.' || ch == '~') {
			out.push_back(static_cast<char>(ch));
		} else {
			out.push_back('%');
			out.push_back(digits[(ch >> 4) & 0xf]);
			out.push_back(digits[ch & 0xf]);
		}
	}
	return out;
}

std::string completion_url(const provider_config_t &provider, bool stream)
{
	const std::string base = trim_slashes(provider.base_url);
	if (provider.protocol == "ollama") {
		return ends_with(base, "/api") ? base + "/chat" :
		                                 base + "/api/chat";
	}
	if (provider.protocol == "gemini_native") {
		const std::string prefix =
		    (ends_with(base, "/v1beta") || ends_with(base, "/v1")) ?
		    base :
		    base + "/v1beta";
		return prefix + "/models/" +
		    percent_encode_path(provider.model) +
		    (stream ? ":streamGenerateContent?alt=sse" :
		              ":generateContent");
	}
	if (provider.protocol == "anthropic_messages") {
		return ends_with(base, "/v1") ? base + "/messages" :
		                                base + "/v1/messages";
	}
	if (!(provider.protocol == "openai_responses"))
		return ends_with(base, "/v1") ? base + "/chat/completions" :
		                                base + "/v1/chat/completions";
	return ends_with(base, "/v1") ? base + "/responses" :
	                                base + "/v1/responses";
}

std::string responses_item_url(const provider_config_t &provider,
    const std::string &response_id, bool cancel)
{
	const std::string base = trim_slashes(provider.base_url);
	const std::string prefix = ends_with(base, "/v1") ? base : base + "/v1";
	return prefix + "/responses/" + percent_encode_path(response_id) +
	    (cancel ? "/cancel" : "");
}

std::string responses_compact_url(const provider_config_t &provider)
{
	const std::string base = trim_slashes(provider.base_url);
	return (ends_with(base, "/v1") ? base : base + "/v1") +
	    "/responses/compact";
}

std::string node_text(acl::json_node *node)
{
	return ::webcool::ai::json_value::string_text(node);
}

long long node_number(acl::json_node *node)
{
	return ::webcool::ai::json_value::number(node);
}

bool node_bool(acl::json_node *node)
{
	return ::webcool::ai::json_value::boolean(node);
}

acl::json_node *object_child(acl::json_node *node, const char *name)
{
	return ::webcool::ai::json_value::object_child(node, name);
}

acl::json_node *first_array_item(acl::json_node *node)
{
	if (node == NULL)
		return NULL;
	acl::json_node *array = node->is_array() ? node : node->get_obj();
	return array != NULL && array->is_array() ? array->first_child() : NULL;
}

acl::json_node *array_value(acl::json_node *node)
{
	return ::webcool::ai::json_value::array_value(node);
}

} // namespace provider_detail

} // namespace ai
} // namespace webcool
