#include <libai_cpp/provider_model.h>
#include "value_bridge.h"
#include <libai/sdk/provider_model.h>
namespace libai
{
namespace cpp
{
// Translate public endpoint options into the private storage/transport configuration.
static libai::ai::provider_config_t native_config(
    const ProviderConfig &source)
{
	libai::ai::provider_config_t target;
	target.id = source.id;
	target.name = source.name;
	target.protocol = source.protocol;
	target.base_url = source.base_url;
	target.model = source.model;
	target.response_state_mode = source.response_state_mode;
	target.responses_store = source.responses_store;
	target.responses_background = source.responses_background;
	target.responses_compact = source.responses_compact;
	target.responses_strict_tools = source.responses_strict_tools;
	target.responses_min_reasoning_effort =
	    source.responses_min_reasoning_effort;
	target.responses_reasoning_summary = source.responses_reasoning_summary;
	target.responses_text_verbosity = source.responses_text_verbosity;
	target.responses_service_tier = source.responses_service_tier;
	target.responses_cache_ttl = source.responses_cache_ttl;
	target.model_profile_json = source.model_profile_json;
	target.qwen_session_cache = source.qwen_session_cache;
	target.openai_organization = source.openai_organization;
	target.openai_project = source.openai_project;
	return target;
}
// Private implementation reuses provider logic compiled against facade-owned types.
struct NativeProviderModel::Impl {
	// Retain the provider configuration and credential in the implementation.
	Impl(const ProviderConfig &config, const std::string &key)
	        : model(native_config(config), key)
	{
	}
	// Adapter belongs to the private implementation namespace.
	libai::provider_model_t model;
};
NativeProviderModel::NativeProviderModel(
    const ProviderConfig &config, const std::string &key)
        : impl_(new Impl(config, key))
{
}
NativeProviderModel::~NativeProviderModel() = default;
ModelResponse NativeProviderModel::complete(
    const ModelRequest &request, const StreamSink &sink)
{
	libai::model_request_t native;
	native.instructions = request.instructions;
	native.profile = libai::parse_model_profile(request.model_profile_json);
	native.messages = detail::convert_values<libai::message_t>(
	    request.messages, detail::native_message);
	native.tools = detail::convert_values<libai::tool_spec_t>(
	    request.tools, detail::spec_value<libai::tool_spec_t, ToolSpec>);
	native.max_output_tokens = request.max_output_tokens;
	native.deadline = request.deadline;
	if (request.cancellation)
		native.cancellation = request.cancellation->impl_->native;
	return detail::public_response(impl_->model.complete(native, sink));
}
}
}
