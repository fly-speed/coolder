#include "stdafx.h"
#include "agent_registry.h"
#include "../prompt/prompt_templates.h"

namespace webcool
{
namespace ai
{
namespace
{

agent_tool_t workspace_tool(const std::string &name,
			    const std::string &description, bool mutating,
			    bool model_enabled, bool requires_file_content,
			    const std::string &authorization)
{
	agent_tool_t tool(name, description, mutating, model_enabled,
			  requires_file_content, authorization);
	const bool has_path = name != "workspace.read_batch" &&
			      name != "workspace.propose_batch" &&
			      name != "workspace.validate";
	const bool path_optional =
		name == "workspace.list" || name == "workspace.search" ||
		name == "code.symbols" || name == "code.references";
	// Batch tools carry their own per-item paths and validation targets the whole
	// selected project. Advertising an unused top-level path confused some
	// OpenAI-compatible models into mixing user-root and project-relative paths.
	if (has_path) {
		tool.parameters.push_back(agent_tool_parameter_t(
			"path", prompt_text(prompt_id::tool_path, false),
			!path_optional));
	}
	if (name == "workspace.search" || name == "code.symbols" ||
	    name == "code.references") {
		tool.parameters.push_back(agent_tool_parameter_t(
			"query",
			name == "workspace.search" ?
				prompt_text(prompt_id::tool_search_query,
					    false) :
				prompt_text(prompt_id::tool_symbol_query,
					    false),
			true));
	}
	if (name == "workspace.read") {
		tool.parameters.push_back(agent_tool_parameter_t(
			"query",
			prompt_text(prompt_id::tool_read_offset, false),
			false));
	}
	if (name == "workspace.create" || name == "workspace.patch" ||
	    name == "workspace.propose" || name == "workspace.replace" ||
	    name == "workspace.patch_set" || name == "workspace.read_batch" ||
	    name == "workspace.propose_batch") {
		tool.parameters.push_back(agent_tool_parameter_t(
			"content",
			name == "workspace.read_batch" ?
				prompt_text(prompt_id::tool_read_batch_content,
					    false) :
				(name == "workspace.propose_batch" ?
					 prompt_text(
						 prompt_id::
							 tool_propose_batch_content,
						 false) :
					 (name == "workspace.replace" ?
						  prompt_text(
							  prompt_id::
								  tool_replacement_content,
							  false) :
						  (name == "workspace.patch_set" ?
							   prompt_text(
								   prompt_id::
									   tool_patch_set_content,
								   false) :
							   prompt_text(
								   prompt_id::
									   tool_file_content,
								   false)))),
			true));
	}
	if (name == "workspace.replace") {
		tool.parameters.push_back(agent_tool_parameter_t(
			"old_text",
			prompt_text(prompt_id::tool_old_text, false), true));
	}
	if (name == "workspace.propose_move") {
		tool.parameters.push_back(agent_tool_parameter_t(
			"target_path",
			prompt_text(prompt_id::tool_target_path, false), true));
	}
	return tool;
}

} // namespace

agent_registry_t::agent_registry_t()
{
	// Registry entries advertise the platform contract. Mutating tools remain
	// declarations until a separate user-confirmed executor is wired to them;
	// listing sandbox.exec here never grants the model process execution.
	agent_definition_t coding;
	coding.id = "coding";
	coding.version = "1.0.0";
	coding.name = prompt_text(prompt_id::coding_name, true);
	coding.description = prompt_text(prompt_id::coding_description, true);
	coding.enabled = true;
	coding.provider_protocols.push_back("openai_responses");
	coding.provider_protocols.push_back("openai_chat");
	coding.provider_protocols.push_back("openai_compatible");
	coding.provider_protocols.push_back("ollama");
	coding.provider_protocols.push_back("anthropic_messages");
	coding.provider_protocols.push_back("gemini_native");
	coding.tools.push_back(workspace_tool(
		"workspace.list", prompt_text(prompt_id::tool_list, true),
		false, true, false, "read"));
	coding.tools.push_back(workspace_tool(
		"workspace.read", prompt_text(prompt_id::tool_read, true),
		false, true, true, "read"));
	coding.tools.push_back(
		workspace_tool("workspace.read_batch",
			       prompt_text(prompt_id::tool_read_batch, true),
			       false, true, true, "read"));
	coding.tools.push_back(workspace_tool(
		"workspace.search", prompt_text(prompt_id::tool_search, true),
		false, true, true, "read"));
	coding.tools.push_back(workspace_tool(
		"workspace.outline", prompt_text(prompt_id::tool_outline, true),
		false, true, true, "read"));
	// `workspace.propose` is intentionally non-mutating: it publishes one
	// complete text-file revision to the live review area, without touching the
	// formal project path before the user accepts it.
	coding.tools.push_back(workspace_tool(
		"workspace.propose", prompt_text(prompt_id::tool_propose, true),
		false, true, true, "proposal"));
	coding.tools.push_back(
		workspace_tool("workspace.propose_batch",
			       prompt_text(prompt_id::tool_propose_batch, true),
			       false, true, true, "proposal"));
	coding.tools.push_back(workspace_tool(
		"workspace.replace", prompt_text(prompt_id::tool_replace, true),
		false, true, true, "proposal"));
	coding.tools.push_back(
		workspace_tool("workspace.patch_set",
			       prompt_text(prompt_id::tool_patch_set, true),
			       false, true, true, "proposal"));
	coding.tools.push_back(
		workspace_tool("workspace.propose_delete",
			       prompt_text(prompt_id::tool_delete, true), false,
			       true, false, "proposal"));
	coding.tools.push_back(
		workspace_tool("workspace.propose_move",
			       prompt_text(prompt_id::tool_move, true), false,
			       true, false, "proposal"));
	coding.tools.push_back(
		workspace_tool("workspace.propose_mkdir",
			       prompt_text(prompt_id::tool_mkdir, true), false,
			       true, false, "proposal"));
	coding.tools.push_back(
		workspace_tool("workspace.validate",
			       prompt_text(prompt_id::tool_validate, true),
			       false, true, false, "draft-validation"));
	coding.tools.push_back(workspace_tool(
		"code.symbols", prompt_text(prompt_id::tool_symbols, true),
		false, true, true, "read"));
	coding.tools.push_back(
		workspace_tool("code.references",
			       prompt_text(prompt_id::tool_references, true),
			       false, true, true, "read"));
	coding.tools.push_back(workspace_tool(
		"workspace.create", prompt_text(prompt_id::tool_create, true),
		true, true, true, "project-create"));
	coding.tools.push_back(workspace_tool(
		"workspace.mkdir",
		prompt_text(prompt_id::tool_create_directory, true), true, true,
		true, "project-create"));
	coding.tools.push_back(workspace_tool(
		"workspace.patch", prompt_text(prompt_id::tool_patch, true),
		true, false, true, "proposal"));
	agent_tool_t sandbox("sandbox.exec",
			     prompt_text(prompt_id::tool_sandbox, true), true,
			     false, false, "user-confirmed-execution");
	sandbox.parameters.push_back(agent_tool_parameter_t(
		"command_id", prompt_text(prompt_id::tool_command_id, false),
		true));
	coding.tools.push_back(sandbox);
	const std::vector<std::pair<std::string, std::string>> browser_tools = {
		{ "browser.status",
		  "Check whether the user paired the affected live browser tab for this project." },
		{ "browser.snapshot",
		  "Read the paired live tab: DOM summary, viewport and iframe evidence. content may be {\"image\":true} only when user enabled screenshots for a vision model." },
		{ "browser.inspect",
		  "Inspect up to four elements, computed styles, rectangles, ancestors and covering elements. query is a CSS selector." },
		{ "browser.overlays",
		  "Find fixed/sticky overlays and iframes, including browser-extension overlays. These may be absent in clean automated browsers." },
		{ "browser.interact",
		  "Interact with the authorized live tab. query=CSS selector; content JSON: {action:click|fill|key|scroll|reload,value?:string,key?:string,y?:number}. Synthetic keys do not perform all native browser defaults. Verify the result." },
		{ "browser.patch_style",
		  "Test a hypothesis with temporary styles. query=CSS selector; content JSON {styles:{property:value}} or {undo:patch_id|all}. Never treat temporary patches as saved source fixes." }
	};
	for (const auto &entry : browser_tools) {
		agent_tool_t tool(entry.first, entry.second, false, true, true,
				  "paired-browser");
		if (entry.first == "browser.inspect" ||
		    entry.first == "browser.interact" ||
		    entry.first == "browser.patch_style")
			tool.parameters.push_back(agent_tool_parameter_t(
				"query",
				"CSS selector in the paired page, not a file path",
				false));
		if (entry.first == "browser.snapshot" ||
		    entry.first == "browser.interact" ||
		    entry.first == "browser.patch_style")
			tool.parameters.push_back(agent_tool_parameter_t(
				"content",
				"JSON object encoded as a string; omit or use {} for defaults",
				false));
		coding.tools.push_back(tool);
	}
	agents_.push_back(coding);
}

const agent_registry_t &agent_registry_t::instance()
{
	static const agent_registry_t registry;
	return registry;
}

const std::vector<agent_definition_t> &agent_registry_t::list() const
{
	return agents_;
}

const agent_definition_t *agent_registry_t::find(const std::string &id) const
{
	for (size_t i = 0; i < agents_.size(); ++i) {
		if (agents_[i].id == id)
			return &agents_[i];
	}
	return NULL;
}

const agent_tool_t *agent_registry_t::find_tool(const agent_definition_t &agent,
						const std::string &name) const
{
	for (size_t i = 0; i < agent.tools.size(); ++i) {
		if (agent.tools[i].name == name)
			return &agent.tools[i];
	}
	return NULL;
}

} // namespace ai
} // namespace webcool
