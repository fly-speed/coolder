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
		if (spec.name.empty() || !handler || tools_.count(spec.name))
			throw std::invalid_argument(
			    "tool needs a unique name and a handler");
		tool_t tool;
		tool.spec = spec;
		tool.execute = std::move(handler);
		tools_.emplace(spec.name, std::move(tool));
		return *this;
	}

	// Execute one bounded model/tool loop while retaining the session transcript.
	run_result_t run(agent_session_t &session, const std::string &input,
	    const run_options_t &options = run_options_t()) const
	{
		sdk_detail::run_engine_t engine(
		    *model_, instructions_, tools_, session, options);
		return engine.execute(input);
	}

private:
	// Model instance owned for the entire agent lifetime.
	std::shared_ptr<model_t> model_;
	// Stable system instructions sent on every model turn.
	std::string instructions_;
	// Named tool schemas and application execution callbacks.
	std::map<std::string, tool_t> tools_;
};

} // namespace libai
