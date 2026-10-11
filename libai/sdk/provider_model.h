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
		target.max_output_tokens =
		    static_cast<long long>(source.max_output_tokens);
		target.store = false;
		target.background = false;
		target.compact_context = false;
		target.strict_tools = false;
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
	// Provider endpoint, protocol and model configuration.
	webcool::ai::provider_config_t config_;
	// Application credential kept in memory; never log or serialize it.
	std::string api_key_;
	// Generate unique IDs for protocols that omit native call identifiers.
	std::size_t sequence_ = 0;
};

} // namespace libai
