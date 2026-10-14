#pragma once
#include <string>
namespace libai
{
namespace cpp
{
// Public model endpoint options independent of storage records and native clients.
struct ProviderConfig {
	// Optional stable identity and display name for this endpoint.
	std::string id, name;
	// Wire protocol, for example openai_chat, and its API base URL.
	std::string protocol, base_url;
	// Provider model identifier used for generation.
	std::string model;
	// Response continuity policy; portable agent calls remain stateless.
	std::string response_state_mode = "auto";
	// Responses privacy and execution controls.
	bool responses_store = true, responses_background = false;
	// Responses context compaction and strict tool schema controls.
	bool responses_compact = true, responses_strict_tools = true;
	// Requested reasoning effort and summary format.
	std::string responses_min_reasoning_effort = "auto",
	            responses_reasoning_summary = "auto";
	// Output verbosity and provider service tier.
	std::string responses_text_verbosity = "medium",
	            responses_service_tier = "auto";
	// Prompt cache retention and optional Qwen session cache.
	std::string responses_cache_ttl = "30m";
	// Persisted, versioned host model capabilities; no core types are exposed.
	std::string model_profile_json = "{\"version\":1}";
	// Opt into the provider's session cache accounting behavior.
	bool qwen_session_cache = false;
	// Optional organization/project routing headers.
	std::string openai_organization, openai_project;
};
}
}
