#include "stdafx.h"
#include "ai_agent_tools_internal.h"

namespace action
{
namespace agent_detail
{

bool path_has_pending_proposal(
	const std::vector<agent_change_proposal_t> *staged_changes,
	const std::string &path)
{
	if (staged_changes == NULL)
		return false;
	for (size_t i = 0; i < staged_changes->size(); ++i) {
		const agent_change_proposal_t &change = (*staged_changes)[i];
		if (change.path == path || (change.operation == "move" &&
					    change.target_path == path)) {
			return true;
		}
	}
	return false;
}

std::string tool_error_json(const std::string &error)
{
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", false);
	root.add_text("error", error.c_str());
	return serialize_json(root);
}

std::string edit_rebase_error(const std::string &error, const std::string &path,
			      const std::string &source, bool chinese)
{
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", false);
	root.add_text("error", error.c_str());
	root.add_text("path", path.c_str());
	root.add_text(
		"file_sha256",
		webcool::ai::agent_workspace_t::content_sha256(source).c_str());
	const std::string page = webcool::ai::utf8_prefix(source, 8192);
	root.add_text("content", page.c_str());
	root.add_number("offset", 0);
	root.add_number("next_offset", static_cast<long long>(page.size()));
	root.add_number("total_bytes", static_cast<long long>(source.size()));
	root.add_bool("eof", page.size() == source.size());
	root.add_text("next_action",
		      prompt_text(prompt_id::edit_rebase_hint, chinese));
	return serialize_json(root);
}

std::string
proposal_error_json(const std::vector<proposal_validation_error_t> &failures,
		    bool chinese)
{
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", false);
	root.add_text("error_code", "proposal_validation_failed");
	root.add_bool("retry_requires_change", true);
	acl::json_node &errors = json.create_array();
	root.add_child("errors", errors);
	static const std::map<std::string, prompt_id> guidance = {
		{ "change_limit", prompt_id::proposal_change_limit },
		{ "parent_missing", prompt_id::proposal_parent_missing },
		{ "parent_unavailable",
		  prompt_id::proposal_parent_unavailable },
		{ "invalid_path", prompt_id::proposal_invalid_path },
		{ "invalid_operation", prompt_id::proposal_invalid_operation },
		{ "invalid_target", prompt_id::proposal_invalid_target },
		{ "invalid_content", prompt_id::proposal_invalid_content },
		{ "explicit_placeholder",
		  prompt_id::proposal_explicit_placeholder },
		{ "duplicate_path", prompt_id::proposal_duplicate_path },
		{ "source_missing", prompt_id::proposal_source_missing },
		{ "source_unreadable", prompt_id::proposal_source_unreadable },
		{ "baseline_changed", prompt_id::proposal_baseline_changed },
		{ "unchanged_content", prompt_id::proposal_unchanged_content },
		{ "target_exists", prompt_id::proposal_target_exists },
		{ "placeholder_not_empty",
		  prompt_id::proposal_placeholder_not_empty },
	};
	for (const auto &failure : failures) {
		acl::json_node &item = errors.add_child(false, true);
		item.add_text("code", failure.code.c_str());
		item.add_text("path", failure.path.c_str());
		item.add_text("related_path", failure.related_path.c_str());
		item.add_text("detail", failure.detail.c_str());
		const char *action =
			prompt_text(guidance.at(failure.code), chinese);
		item.add_text("action", action);
		if (&failure == &failures.front())
			root.add_text("error", action);
	}
	return serialize_json(root);
}

// Read server-generated diagnostics, including the internal native batch envelope.
void collect_proposal_failure_causes(const std::string &result,
				     std::vector<std::string> &causes,
				     size_t depth)
{
	if (depth > 4)
		return;
	acl::json parsed(result.c_str());
	if (!parsed.finish())
		return;
	acl::json_node &root = parsed.get_root();
	if (json_text(root["error_code"]) == "proposal_validation_failed") {
		acl::json_node *errors = json_array_node(root["errors"]);
		if (errors != NULL)
			for (acl::json_node *item = errors->first_child();
			     item != NULL; item = errors->next_child()) {
				acl::json_node *object =
					item->is_object() ? item :
							    item->get_obj();
				if (object == NULL)
					continue;
				const std::string code =
					json_text((*object)["code"]);
				const std::string path =
					json_text((*object)["path"]);
				const std::string related =
					json_text((*object)["related_path"]);
				// A missing parent is the same blocker for every file under it.
				causes.push_back(code + "\n" +
						 (code == "parent_missing" ?
							  related :
							  path) +
						 "\n" + related);
			}
	}
	const std::string cause = json_text(root["cause"]);
	if (!cause.empty())
		collect_proposal_failure_causes(cause, causes, depth + 1);
	acl::json_node *results = json_array_node(root["results"]);
	if (results != NULL)
		for (acl::json_node *item = results->first_child();
		     item != NULL; item = results->next_child()) {
			acl::json_node *object =
				item->is_object() ? item : item->get_obj();
			if (object != NULL)
				collect_proposal_failure_causes(
					json_text((*object)["result"]), causes,
					depth + 1);
		}
}

bool is_incremental_proposal_tool(const std::string &name)
{
	return name == "workspace.propose" ||
	       name == "workspace.propose_batch" ||
	       name == "workspace.replace" || name == "workspace.patch_set" ||
	       name == "workspace.propose_delete" ||
	       name == "workspace.propose_move" ||
	       name == "workspace.propose_mkdir";
}

bool request_contains_only_proposals(const agent_tool_request_t &request)
{
	if (is_incremental_proposal_tool(request.name))
		return true;
	if (request.name != "workspace.execute_batch")
		return false;

	// `workspace.execute_batch` is an internal envelope synthesized when one
	// assistant turn contains several native tool calls. It is not inherently a
	// mutation: it can contain only reads. Inspect every child before allowing the
	// envelope through a proposal-only boundary, otherwise parallel reads bypass
	// the no-progress guard and are incorrectly counted as delivery attempts.
	acl::json parsed(request.content.c_str());
	acl::json_node *items =
		parsed.finish() ? json_array_node(&parsed.get_root()) : NULL;
	if (items == NULL || items->first_child() == NULL)
		return false;
	for (acl::json_node *item = items->first_child(); item != NULL;
	     item = items->next_child()) {
		acl::json_node *object =
			item->is_object() ? item : item->get_obj();
		if (object == NULL || !is_incremental_proposal_tool(
					      json_text((*object)["name"]))) {
			return false;
		}
	}
	return true;
}

bool is_parallel_read_tool(const std::string &name)
{
	return name == "workspace.list" || name == "workspace.read" ||
	       name == "workspace.read_batch" || name == "workspace.search" ||
	       name == "workspace.outline" || name == "code.symbols" ||
	       name == "code.references";
}

agent_tool_request_t
completion_tool_request(const webcool::ai::completion_tool_call_t &call)
{
	agent_tool_request_t request;
	request.name = call.name;
	request.path = call.path;
	request.query = call.query;
	request.old_text = call.old_text;
	request.target_path = call.target_path;
	request.content = call.content;
	request.content_present = call.content_present;
	return request;
}

agent_tool_request_t merge_completion_tool_calls(
	const std::vector<webcool::ai::completion_tool_call_t> &calls)
{
	if (calls.size() == 1)
		return completion_tool_request(calls.front());
	agent_tool_request_t request;
	request.name = "workspace.execute_batch";
	acl::json json;
	acl::json_node &items = json.create_array();
	for (size_t i = 0; i < calls.size(); ++i) {
		acl::json_node &item = items.add_child(false, true);
		item.add_text("name", calls[i].name.c_str());
		if (!calls[i].path.empty())
			item.add_text("path", calls[i].path.c_str());
		if (!calls[i].query.empty())
			item.add_text("query", calls[i].query.c_str());
		if (!calls[i].old_text.empty()) {
			item.add_text("old_text", calls[i].old_text.c_str());
		}
		if (!calls[i].target_path.empty()) {
			item.add_text("target_path",
				      calls[i].target_path.c_str());
		}
		if (calls[i].content_present || !calls[i].content.empty()) {
			item.add_text("content", calls[i].content.c_str());
		}
	}
	const acl::string &serialized = items.to_string();
	request.content.assign(serialized.c_str(), serialized.size());
	return request;
}

bool build_native_tool_outputs(
	const std::vector<webcool::ai::completion_tool_call_t> &calls,
	const std::string &combined_result,
	std::vector<webcool::ai::completion_tool_output_t> &outputs)
{
	outputs.clear();
	if (calls.size() == 1) {
		webcool::ai::completion_tool_output_t output;
		output.call_id = calls.front().id.empty() ? "webcool_call_1" :
							    calls.front().id;
		output.output = combined_result;
		outputs.push_back(output);
		return true;
	}

	// execute_batch returns one bounded envelope. Split it back into the matching
	// per-call outputs required by the Responses API instead of sending every call
	// a duplicate copy of the whole batch (which wastes context tokens).
	acl::json parsed(combined_result.c_str());
	acl::json_node *results =
		parsed.finish() ? json_array_node(parsed["results"]) : NULL;
	if (results == NULL && parsed["ok"] != NULL &&
	    !json_bool(parsed["ok"], true)) {
		// A rolled-back/failed batch still answers every native call. Omitting
		// these outputs would replay the previous successful history unchanged.
		for (size_t i = 0; i < calls.size(); ++i) {
			webcool::ai::completion_tool_output_t output;
			output.call_id = calls[i].id.empty() ?
						 "webcool_call_" +
							 std::to_string(i + 1) :
						 calls[i].id;
			output.output = combined_result;
			outputs.push_back(output);
		}
		return !calls.empty();
	}
	acl::json_node *item = results ? results->first_child() : NULL;
	for (size_t i = 0; i < calls.size(); ++i) {
		if (item == NULL) {
			outputs.clear();
			return false;
		}
		acl::json_node *object =
			item->is_object() ? item : item->get_obj();
		if (object == NULL) {
			outputs.clear();
			return false;
		}
		webcool::ai::completion_tool_output_t output;
		output.call_id =
			calls[i].id.empty() ?
				"webcool_call_" + std::to_string(i + 1) :
				calls[i].id;
		output.output = json_text((*object)["result"]);
		outputs.push_back(output);
		item = results->next_child();
	}
	return item == NULL;
}

bool completion_calls_have_ids(
	const std::vector<webcool::ai::completion_tool_call_t> &calls)
{
	if (calls.empty())
		return false;
	for (size_t i = 0; i < calls.size(); ++i) {
		if (calls[i].id.empty())
			return false;
	}
	return true;
}

// Preserve source pages: later compaction may need them. Coverage controls
// progress credit and supplies an immediate actionable hint on redundant reads.

}
}
