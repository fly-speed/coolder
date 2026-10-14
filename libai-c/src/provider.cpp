#include "bridge_internal.h"
#ifdef LIBAI_CODING_FULL
#include <libai/sdk/provider_model.h>
#include <libai/common/json_values.h>
#endif
extern "C" {
libai_agent *libai_agent_create_provider(
    const char *config, const char *api_key, const char *instructions)
{
	bridge_error("");
#ifdef LIBAI_CODING_FULL
	try {
		if (!config || !api_key || !instructions)
			throw std::invalid_argument(
			    "provider arguments must be non-null");
		const std::string json(config);
		if (json.size() > 1024 * 1024 ||
		    json.find("\\u0000") != std::string::npos)
			throw std::invalid_argument(
			    "invalid provider configuration");
		acl::json input(json.c_str());
		if (!input.finish() || !input.get_root().is_object())
			throw std::invalid_argument(
			    "provider config must be a JSON object");
		auto &node = input.get_root();
		libai::ai::provider_config_t p;
		p.id =
		    (node["id"] ? libai::ai::json_values::text(node["id"]) :
		                  p.id);
		p.name = (node["name"] ?
		        libai::ai::json_values::text(node["name"]) :
		        p.name);
		if (node["model_profile_json"]) {
			if (!node["model_profile_json"]->is_string())
				throw std::invalid_argument("model_profile_json must be a string");
			p.model_profile_json=libai::parse_model_profile(libai::ai::json_values::text(node["model_profile_json"])).json;
		}
		p.protocol = (node["protocol"] ?
		        libai::ai::json_values::text(node["protocol"]) :
		        p.protocol);
		p.base_url = (node["base_url"] ?
		        libai::ai::json_values::text(node["base_url"]) :
		        p.base_url);
		p.model = (node["model"] ?
		        libai::ai::json_values::text(node["model"]) :
		        p.model);
		p.response_state_mode = (node["response_state_mode"] ?
		        libai::ai::json_values::text(
		            node["response_state_mode"]) :
		        p.response_state_mode);
		p.responses_min_reasoning_effort =
		    (node["responses_min_reasoning_effort"] ?
		            libai::ai::json_values::text(
		                node["responses_min_reasoning_effort"]) :
		            p.responses_min_reasoning_effort);
		p.responses_reasoning_summary =
		    (node["responses_reasoning_summary"] ?
		            libai::ai::json_values::text(
		                node["responses_reasoning_summary"]) :
		            p.responses_reasoning_summary);
		p.responses_text_verbosity = (node["responses_text_verbosity"] ?
		        libai::ai::json_values::text(
		            node["responses_text_verbosity"]) :
		        p.responses_text_verbosity);
		p.responses_service_tier = (node["responses_service_tier"] ?
		        libai::ai::json_values::text(
		            node["responses_service_tier"]) :
		        p.responses_service_tier);
		p.responses_cache_ttl = (node["responses_cache_ttl"] ?
		        libai::ai::json_values::text(
		            node["responses_cache_ttl"]) :
		        p.responses_cache_ttl);
		p.openai_organization = (node["openai_organization"] ?
		        libai::ai::json_values::text(
		            node["openai_organization"]) :
		        p.openai_organization);
		p.openai_project = (node["openai_project"] ?
		        libai::ai::json_values::text(node["openai_project"]) :
		        p.openai_project);
		p.responses_store = libai::ai::json_values::boolean(
		    node["responses_store"], p.responses_store);
		p.responses_background = libai::ai::json_values::boolean(
		    node["responses_background"], p.responses_background);
		p.responses_compact = libai::ai::json_values::boolean(
		    node["responses_compact"], p.responses_compact);
		p.responses_strict_tools = libai::ai::json_values::boolean(
		    node["responses_strict_tools"], p.responses_strict_tools);
		p.qwen_session_cache = libai::ai::json_values::boolean(
		    node["qwen_session_cache"], p.qwen_session_cache);
		if (p.protocol.empty() || p.base_url.empty() || p.model.empty())
			throw std::invalid_argument(
			    "provider protocol, base_url and model are required");
		std::unique_ptr<libai_agent> agent(new libai_agent);
		agent->model =
		    std::make_shared<libai::provider_model_t>(p, api_key);
		agent->runtime.reset(
		    new libai::agent_t(agent->model, instructions));
		return agent.release();
	} catch (const std::exception &error) {
		bridge_error(error.what());
	} catch (...) {
		bridge_error("provider agent creation failed");
	}
#else
	(void)config;
	(void)api_key;
	(void)instructions;
	bridge_error("Provider requires LIBAI_CODING_FULL=ON");
#endif
	return nullptr;
}
}
