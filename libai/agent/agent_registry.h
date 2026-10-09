#pragma once

#include <string>
#include <vector>

namespace webcool
{
namespace ai
{

// Provider-neutral string parameter used to build native function schemas.
// More types can be added without coupling the agent registry to any vendor.
struct agent_tool_parameter_t {
	// Initialize agent tool parameter state from the supplied arguments.
	agent_tool_parameter_t(const std::string &parameter_name,
	    const std::string &parameter_description, bool parameter_required)
	        : name(parameter_name)
	        , description(parameter_description)
	        , required(parameter_required)
	{
	}
	// Name used to identify this item in its containing collection.
	std::string name;
	// Human-readable explanation presented to the model or user.
	std::string description;
	// Whether callers must supply this parameter.
	bool required;
};

// Describes a capability for presentation, provider schemas and policy checks.
// `model_enabled` is separate from declaration: user-confirmed tools remain
// visible in the platform contract without being callable by a model turn.
struct agent_tool_t {
	// Initialize agent tool state from the supplied arguments.
	agent_tool_t(const std::string &tool_name,
	    const std::string &tool_description, bool tool_mutating,
	    bool enabled_for_model, bool needs_file_content,
	    const std::string &tool_authorization)
	        : name(tool_name)
	        , description(tool_description)
	        , mutating(tool_mutating)
	        , model_enabled(enabled_for_model)
	        , requires_file_content(needs_file_content)
	        , authorization(tool_authorization)
	{
	}
	// Name used to identify this item in its containing collection.
	std::string name;
	// Human-readable explanation presented to the model or user.
	std::string description;
	// Whether the tool can modify staged workspace state.
	bool mutating;
	// Whether this tool is exposed in the model's tool catalog.
	bool model_enabled;
	// Whether execution requires permission to read file contents.
	bool requires_file_content;
	// Stable values: read, project-create, proposal, user-confirmed-execution.
	std::string authorization;
	// Parameter schema used to describe the tool to providers.
	std::vector<agent_tool_parameter_t> parameters;
};

// Stable metadata for one agent type. Provider settings reference protocols,
// not concrete SDK classes, so new agents and providers remain independent.
struct agent_definition_t {
	// Identifier used to look up this record.
	std::string id;
	// Version of the stored record or agent definition.
	std::string version;
	// Name used to identify this item in its containing collection.
	std::string name;
	// Human-readable explanation presented to the model or user.
	std::string description;
	// Whether this capability is enabled by its configuration.
	bool enabled;
	// Provider protocols supported by this agent definition.
	std::vector<std::string> provider_protocols;
	// Tool definitions available to this request or agent.
	std::vector<agent_tool_t> tools;
};

// Process-wide immutable registry. Adding another agent only adds a definition
// here; it does not change per-user provider storage or workspace ownership.
class agent_registry_t {
public:
	// Return the process-wide immutable agent registry.
	static const agent_registry_t &instance();
	// Return the immutable collection of registered agent definitions.
	const std::vector<agent_definition_t> &list() const;
	// Look up the requested definition; return null when it is absent.
	const agent_definition_t *find(const std::string &id) const;
	// Look up a named tool within the selected agent definition.
	const agent_tool_t *find_tool(
	    const agent_definition_t &agent, const std::string &name) const;

private:
	// Initialize agent registry state from the supplied arguments.
	agent_registry_t();
	// Registered agent definitions owned by this registry.
	std::vector<agent_definition_t> agents_;
};

} // namespace ai
} // namespace webcool
