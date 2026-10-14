#pragma once
// Public portable agent facade; implementation remains header-only.
#include "agent_types.h"
#include "run_engine.h"

namespace libai
{
// Application-facing facade for a model and an extensible tool catalog.
class agent_t {
public:
	// Bind the model lifetime and stable instructions to this agent.
	explicit agent_t(std::shared_ptr<model_t> model,
	    const std::string &instructions = "")
	        : model_(std::move(model))
	        , instructions_(instructions)
	{
		if (!model_)
			throw std::invalid_argument("model is null");
	}
	// Register a unique application capability; reject empty handlers.
	agent_t &add_tool(const tool_spec_t &spec, tool_handler_t handler)
	{
		if (spec.name.empty() || !handler || tools_.count(spec.name) ||
		    spec.execution_revision.empty() || spec.execution_revision.size()>128)
			throw std::invalid_argument(
			    "tool needs a unique name and a handler");
		tool_t tool;
		tool.spec = spec;
		tool.schema = std::make_shared<json_schema_t>(spec.parameters);
		tool.execute = std::move(handler);
		tools_.emplace(spec.name, std::move(tool));
		return *this;
	}

	// Replace capabilities only between runs; invalid profiles leave prior state intact.
	agent_t &configure_model(const std::string &profile_json)
	{
		profile_ = parse_model_profile(profile_json);
		return *this;
	}

    // Validate configuration before replacement. Empty schema disables final JSON validation.
    agent_t &configure_output(const std::string &schema, std::size_t repairs=1) {
        if(repairs>8)throw std::invalid_argument("output repair feedback limit exceeds 8");
        if(!schema.empty()){json_schema_t checked(schema);}
        output_schema_=schema;output_repairs_=repairs;output_configured_=true;
        return *this;
    }

	// Execute one bounded model/tool loop while retaining the session transcript.
	run_result_t run(agent_session_t &session, const std::string &input,
	    const run_options_t &options = run_options_t()) const
	{
		auto effective = options;
        if(output_configured_){effective.output_schema=output_schema_;effective.max_output_repairs=output_repairs_;}
		effective.profile = profile_;
		effective.max_tokens_per_call = std::min(options.max_tokens_per_call, profile_.max_output_tokens);
		effective.max_context_bytes = std::min(options.max_context_bytes, profile_.max_context_bytes);
		sdk_detail::run_engine_t engine(
		    *model_, instructions_, tools_, session, effective);
		return engine.execute(input);
	}

private:
    std::string output_schema_;
    std::size_t output_repairs_=1;
    bool output_configured_=false;
	// Host-selected limits and capability declarations.
	model_profile_t profile_;
	// Model instance owned for the entire agent lifetime.
	std::shared_ptr<model_t> model_;
	// Stable system instructions sent on every model turn.
	std::string instructions_;
	// Named tool schemas and application execution callbacks.
	std::map<std::string, tool_t> tools_;
};

} // namespace libai
