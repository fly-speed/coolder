#pragma once
#include "agent.h"
#include "../provider/ai_provider_client.h"

namespace libai
{

// Reuses libai's native HTTP clients. The host initializes ACL/fiber as before.
// Keep this model alive for the agent lifetime; never log the credential.
class provider_model_t : public model_t {
public:
	// Retain provider configuration and the application-supplied credential.
	provider_model_t(const webcool::ai::provider_config_t &config,
	    const std::string &api_key)
	        : config_(config)
	        , api_key_(api_key)
	{
	}

	// Translate a portable request into the existing native HTTP request.
	static webcool::ai::completion_request_t request(
	    const model_request_t &source)
	{
		if (source.max_output_tokens >
		    static_cast<std::size_t>(
		        std::numeric_limits<long long>::max()))
			throw std::invalid_argument(
			    "provider token limit is too large");
		webcool::ai::completion_request_t target;
		target.system_prompt = source.instructions;
		target.messages = source.messages;
		// The native transport validates this legacy field even for typed transcripts.
		// Protocol writers use messages above, preserving tool exchanges without duplication.
		target.user_prompt = "Continue.";
		for (const auto &message : source.messages) {
			if (message.role == message_t::role_t::user &&
			    !message.text.empty())
				target.user_prompt = message.text;
		}
		target.max_output_tokens =
		    static_cast<long long>(source.max_output_tokens);
		target.store = false;
		target.background = false;
		target.compact_context = false;
		target.strict_tools = false;
		target.allow_multiple_tool_calls = source.profile.multiple_tool_calls;
		std::set<std::string> names;
		for (const auto &spec : source.tools) {
			std::string wire = spec.name;
			std::replace(wire.begin(), wire.end(), '.', '_');
			if (!names.insert(wire).second)
				throw std::invalid_argument(
				    "tool names collide on provider wire");
			webcool::ai::agent_tool_t tool(spec.name,
			    spec.description, false, true, false,
			    "application");
			tool.parameters_json = spec.parameters;
			target.tools.push_back(tool);
		}
		return target;
	}

	// Perform the native request and normalize calls, text and usage.
	model_response_t complete(
	    const model_request_t &source, const stream_sink_t &sink) override
	{
		// Forward cooperative stop requests and text to the native transport.
		class observer_t
		        : public webcool::ai::completion_stream_observer_t {
		public:
			// Borrow request boundaries and the synchronous stream sink.
			observer_t(const model_request_t &request,
			    const stream_sink_t &sink)
			        : request_(request)
			        , sink_(sink)
			{
			}
			// Check the run boundary between native response fragments.
			bool cancel_requested() const override
			{
				return (request_.cancellation &&
				           request_.cancellation
				               ->cancelled()) ||
				    std::chrono::steady_clock::now() >=
				    request_.deadline;
			}
			// Forward text only while the run remains active.
			bool on_text_delta(const std::string &delta) override
			{
				return !cancel_requested() &&
				    (!sink_ || sink_(delta));
			}

		private:
			const model_request_t &request_;
			const stream_sink_t &sink_;
		} observer(source, sink);
		auto input = request(source);
		input.images = images_;
		input.reasoning_effort = reasoning_effort_;
		input.transport_session =
		    webcool::ai::provider_client_t::create_transport_session();
		webcool::ai::completion_result_t output;
		std::string error;
		model_response_t result;
		if (!webcool::ai::provider_client_t::complete(
		        config_, api_key_, input, output, error, &observer)) {
			result.error =
			    error.empty() ? "provider request failed" : error;
			result.retryable = output.retryable_error;
		}
		last_completion_ = output;
		result.text = output.text;
		result.reasoning = output.reasoning;
		result.input_tokens = static_cast<std::size_t>(
		    std::max(0LL, output.input_tokens));
		result.output_tokens = static_cast<std::size_t>(
		    std::max(0LL, output.output_tokens));
		for (const auto &native : output.tool_calls) {
			tool_call_t call;
			// Some native protocols do not issue call IDs (notably Gemini).
			call.id = native.id.empty() ?
			    "libai_" + std::to_string(sequence_++) :
			    native.id;
			call.name = restored_name(source.tools, native.name);
			call.arguments = native.arguments_json.empty() ?
			    "{}" :
			    native.arguments_json;
			result.calls.push_back(call);
		}
		return result;
	}

	// Supply bounded in-memory images for provider-native vision requests.
	void set_reasoning_effort(const std::string &effort)
	{
		reasoning_effort_ = effort;
	}

	void set_images(const std::vector<webcool::ai::completion_image_t> &images)
	{
		images_ = images;
	}

	// Last native completion metadata for generic service usage accounting.
	const webcool::ai::completion_result_t &last_completion() const
	{
		return last_completion_;
	}

private:
	// Reverse wire-name normalization using this request's advertised tools.
	static std::string restored_name(
	    const std::vector<tool_spec_t> &tools, const std::string &name)
	{
		for (const auto &spec : tools) {
			std::string canonical = spec.name;
			std::replace(
			    canonical.begin(), canonical.end(), '_', '.');
			if (canonical == name)
				return spec.name;
		}
		return name;
	}
	// Optional provider-native image attachments.
	std::vector<webcool::ai::completion_image_t> images_;
	std::string reasoning_effort_;
	// Last response usage and timing; no credential is included.
	webcool::ai::completion_result_t last_completion_;
	// Provider endpoint, protocol and model configuration.
	webcool::ai::provider_config_t config_;
	// Application credential kept in memory; never log or serialize it.
	std::string api_key_;
	// Generate unique IDs for protocols that omit native call identifiers.
	std::size_t sequence_ = 0;
};

} // namespace libai
