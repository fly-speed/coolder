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
	agent_tool_parameter_t(const std::string &parameter_name,
			       const std::string &parameter_description,
			       bool parameter_required)
		: name(parameter_name)
		, description(parameter_description)
		, required(parameter_required)
	{
	}
	std::string name;
	std::string description;
	bool required;
};

// Describes a capability for presentation, provider schemas and policy checks.
// `model_enabled` is separate from declaration: user-confirmed tools remain
// visible in the platform contract without being callable by a model turn.
struct agent_tool_t {
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
	std::string name;
	std::string description;
	bool mutating;
	bool model_enabled;
	bool requires_file_content;
	// Stable values: read, project-create, proposal, user-confirmed-execution.
	std::string authorization;
	std::vector<agent_tool_parameter_t> parameters;
};

// Stable metadata for one agent type. Provider settings reference protocols,
// not concrete SDK classes, so new agents and providers remain independent.
struct agent_definition_t {
	std::string id;
	std::string version;
	std::string name;
	std::string description;
	bool enabled;
	std::vector<std::string> provider_protocols;
	std::vector<agent_tool_t> tools;
};

// Process-wide immutable registry. Adding another agent only adds a definition
// here; it does not change per-user provider storage or workspace ownership.
class agent_registry_t {
public:
	static const agent_registry_t &instance();
	const std::vector<agent_definition_t> &list() const;
	const agent_definition_t *find(const std::string &id) const;
	const agent_tool_t *find_tool(const agent_definition_t &agent,
				      const std::string &name) const;

private:
	agent_registry_t();
	std::vector<agent_definition_t> agents_;
};

} // namespace ai
} // namespace webcool
