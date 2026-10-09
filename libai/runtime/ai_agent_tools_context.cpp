#include "stdafx.h"
#include "ai_agent_tools_internal.h"

namespace action
{
namespace agent_detail
{

std::string annotate_read_coverage(const std::string &name,
    const std::string &result, webcool::ai::agent_read_coverage_t &coverage,
    bool chinese)
{
	acl::json parsed(result.c_str());
	if (!parsed.finish())
		return result;
	if (name == "workspace.execute_batch") {
		acl::json_node *results = json_array_node(parsed["results"]);
		for (acl::json_node *item = results ? results->first_child() :
		                                      NULL;
		     item; item = results->next_child()) {
			acl::json_node *object =
			    item->is_object() ? item : item->get_obj();
			if (!(object && (*object)["result"]))
				continue;
			(*object)["result"]->set_text(annotate_read_coverage(
			    json_text((*object)["name"]),
			    json_text((*object)["result"]), coverage, chinese)
			                                  .c_str());
		}
	} else if (name == "workspace.read" || name == "workspace.read_batch") {
		auto annotate = [&](acl::json_node &page) {
			const std::string version =
			    json_text(page["file_sha256"]);
			if (version.empty() || page["content"] == NULL)
				return;
			const size_t begin =
			    static_cast<size_t>(json_number(page["offset"], 0));
			const size_t length = json_text(page["content"]).size();
			const size_t added =
			    coverage.observe(json_text(page["path"]), version,
			        begin, begin + length);
			page.add_number(
			    "new_read_bytes", static_cast<long long>(added));
			if (length && added < length && added <= length / 10) {
				page.add_bool("low_value_read", true);
				page.add_text("reuse_guidance",
				    prompt_text(
				        prompt_id::read_overlap_guidance,
				        chinese));
			}
		};
		acl::json_node *files = json_array_node(parsed["files"]);
		for (acl::json_node *item = files ? files->first_child() : NULL;
		     item; item = files->next_child()) {
			acl::json_node *object =
			    item->is_object() ? item : item->get_obj();
			if (!object)
				continue;
			annotate(*object);
		}
		if (!files)
			annotate(parsed.get_root());
	}
	return serialize_json(parsed.get_root());
}

std::string workspace_inspection_error(
    const std::string &err, const std::string &path, bool chinese)
{
	if (err != "workspace path does not exist")
		return tool_error_json(err);
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", false);
	root.add_text("error", err.c_str());
	root.add_text("path", path.c_str());
	root.add_text("path_state", "missing");
	root.add_text("next_action",
	    prompt_text(prompt_id::missing_path_guidance, chinese));
	return serialize_json(root);
}

// Batch boundaries and ordering are transport details, not new evidence.
void collect_observation_signatures(const std::string &name,
    const std::string &result, std::vector<std::string> &signatures)
{
	acl::json parsed(result.c_str());
	if (!parsed.finish())
		return;
	// Reusing an old validation report executes nothing and adds no evidence.
	// Metadata such as reuse_note must not reset the no-progress counter.
	if (name == "workspace.validate" &&
	    json_bool(parsed["validation_reused"], false))
		return;
	if (name == "workspace.execute_batch") {
		acl::json_node *results = json_array_node(parsed["results"]);
		for (acl::json_node *item = results ? results->first_child() :
		                                      NULL;
		     item; item = results->next_child()) {
			acl::json_node *object =
			    item->is_object() ? item : item->get_obj();
			if (!object)
				continue;
			collect_observation_signatures(
			    json_text((*object)["name"]),
			    json_text((*object)["result"]), signatures);
		}
		return;
	}
	if (name == "workspace.validate" &&
	    !json_bool(parsed["validation_passed"], true)) {
		// Compiler timings/progress are not new evidence of a different failure.
		std::string failure = json_text(parsed["failed_command_id"]);
		acl::json_node *commands = json_array_node(parsed["commands"]);
		for (acl::json_node *item = commands ? commands->first_child() :
		                                       NULL;
		     item; item = commands->next_child()) {
			acl::json_node *command =
			    item->is_object() ? item : item->get_obj();
			if (!command || json_bool((*command)["passed"], false))
				continue;
			failure += ":" + json_text((*command)["error"]) + ":" +
			    json_text((*command)["exit_code"]);
			acl::json_node *diagnostics =
			    json_array_node((*command)["diagnostics"]);
			if (!diagnostics)
				continue;
			failure += serialize_json(*diagnostics);
		}
		signatures.push_back("validation-failure:" + failure);
		return;
	}
	acl::json_node *files = json_array_node(parsed["files"]);
	if (files != NULL) {
		for (acl::json_node *item = files->first_child(); item != NULL;
		     item = files->next_child()) {
			acl::json_node *object =
			    item->is_object() ? item : item->get_obj();
			if (object == NULL ||
			    json_bool((*object)["low_value_read"], false))
				continue;
			signatures.push_back(
			    "read:" + json_text((*object)["path"]) + ":" +
			    json_text((*object)["file_sha256"]) + ":" +
			    json_text((*object)["offset"]) + ":" +
			    webcool::ai::agent_workspace_t::content_sha256(
			        json_text((*object)["content"])));
		}
	} else if (name == "workspace.read" && parsed["content"] != NULL) {
		if (json_bool(parsed["low_value_read"], false))
			return;
		signatures.push_back("read:" + json_text(parsed["path"]) + ":" +
		    json_text(parsed["file_sha256"]) + ":" +
		    json_text(parsed["offset"]) + ":" +
		    webcool::ai::agent_workspace_t::content_sha256(
		        json_text(parsed["content"])));
	} else {
		signatures.push_back(name + ":" +
		    webcool::ai::agent_workspace_t::content_sha256(result));
	}
}

// Extract only failure evidence, not timestamps, build paths or PASS lines.
bool repeated_repair_failure(const std::string &report,
    const std::string &draft, webcool::ai::repair_failure_tracker_t &tracker)
{
	acl::json parsed(report.c_str());
	if (!parsed.finish())
		return false;
	if (json_bool(parsed["validation_reused"], false))
		return false;
	if (json_bool(parsed["validation_passed"], false)) {
		tracker.clear();
		return false;
	}
	const std::string failed_command =
	    json_text(parsed["failed_command_id"]);
	std::set<std::string> failures;
	acl::json_node *commands = json_array_node(parsed["commands"]);
	for (acl::json_node *item = commands ? commands->first_child() : NULL;
	     item; item = commands->next_child()) {
		acl::json_node *command =
		    item->is_object() ? item : item->get_obj();
		if (!command ||
		    json_text((*command)["command_id"]) != failed_command)
			continue;
		const auto extracted = webcool::ai::repair_failure_signatures(
		    json_text((*command)["diagnostic"]));
		failures.insert(extracted.begin(), extracted.end());
	}
	return tracker.observe(failed_command, draft, failures, false);
}

// A repeated failure requires fresh evidence before another mutation. Batch
// envelopes are inspected before execution, so a mixed read/write batch cannot
// bypass the review boundary or partially modify the draft.
bool repair_review_read_only(const agent_tool_request_t &request)
{
	if (is_parallel_read_tool(request.name) ||
	    request.name == "browser.status" ||
	    request.name == "browser.snapshot" ||
	    request.name == "browser.inspect" ||
	    request.name == "browser.overlays")
		return true;
	if (request.name != "workspace.execute_batch")
		return false;
	acl::json parsed(request.content.c_str());
	acl::json_node *items =
	    parsed.finish() ? json_array_node(&parsed.get_root()) : NULL;
	if (!items)
		return false;
	for (acl::json_node *item = items->first_child(); item;
	     item = items->next_child()) {
		acl::json_node *object =
		    item->is_object() ? item : item->get_obj();
		if (!(!object ||
		        !is_parallel_read_tool(json_text((*object)["name"]))))
			continue;
		return false;
	}
	return true;
}

void consume_repair_reads(const std::string &name, const std::string &result,
    std::map<std::string, std::string> &required)
{
	acl::json parsed(result.c_str());
	if (!parsed.finish())
		return;
	if (name == "workspace.execute_batch") {
		acl::json_node *results = json_array_node(parsed["results"]);
		for (acl::json_node *item = results ? results->first_child() :
		                                      NULL;
		     item; item = results->next_child()) {
			acl::json_node *object =
			    item->is_object() ? item : item->get_obj();
			if (!object)
				continue;
			consume_repair_reads(json_text((*object)["name"]),
			    json_text((*object)["result"]), required);
		}
		return;
	}
	if (name != "workspace.read" && name != "workspace.read_batch")
		return;
	auto consume = [&](acl::json_node &page) {
		const auto found = required.find(json_text(page["path"]));
		if (found != required.end() && page["content"] != NULL &&
		    (!json_text(page["content"]).empty() ||
		        json_number(page["total_bytes"], -1) == 0) &&
		    found->second == json_text(page["file_sha256"]))
			required.erase(found);
	};
	acl::json_node *files = json_array_node(parsed["files"]);
	if (!files)
		consume(parsed.get_root());
	else
		for (acl::json_node *item = files->first_child(); item;
		     item = files->next_child()) {
			acl::json_node *object =
			    item->is_object() ? item : item->get_obj();
			if (!object)
				continue;
			consume(*object);
		}
}

std::string repair_review_required(
    const std::map<std::string, std::string> &required, bool chinese)
{
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", false);
	root.add_text("error_code", "repair_review_required");
	root.add_text(
	    "error", prompt_text(prompt_id::repair_review_required, chinese));
	acl::json_node &paths = json.create_array();
	for (const auto &entry : required) {
		acl::json_node &file = json.create_node();
		file.add_text("path", entry.first.c_str());
		file.add_text("file_sha256", entry.second.c_str());
		paths.add_child(&file);
	}
	root.add_child("required_reads", &paths);
	return serialize_json(root);
}

std::string batch_read_continuation(
    const std::string &name, const std::string &result)
{
	acl::json parsed(result.c_str());
	if (!parsed.finish())
		return "";
	if (name == "workspace.read_batch") {
		const std::string next = json_text(parsed["next_content"]);
		if (!(next.empty() || next == "[]"))
			return "\n<batch_read_continuation>" + next +
			    "</batch_read_continuation>\n"
			    "Pass this array as workspace.read_batch content; do not restart the original batch.\n";
		return "";
	}
	std::string guidance;
	if (!(name == "workspace.execute_batch"))
		return guidance;
	auto *results = json_array_node(parsed["results"]);
	for (auto *item = results ? results->first_child() : NULL; item;
	     item = results->next_child()) {
		auto *object = item->is_object() ? item : item->get_obj();
		if (!object)
			continue;
		guidance +=
		    batch_read_continuation(json_text((*object)["name"]),
		        json_text((*object)["result"]));
	}

	return guidance;
}

bool remember_read_page(
    acl::json_node &page, webcool::ai::agent_read_context_t &context)
{
	const std::string path = json_text(page["path"]);
	const std::string version = json_text(page["file_sha256"]);
	if (!(page["content"] == NULL || version.empty()))
		return context.put(path, version,
		    static_cast<size_t>(json_number(page["offset"], 0)),
		    serialize_json(page));
	return false;
}

bool remember_read_result(const std::string &name, const std::string &result,
    webcool::ai::agent_read_context_t &context)
{
	if (name != "workspace.read" && name != "workspace.read_batch" &&
	    name != "workspace.execute_batch")
		return false;
	acl::json parsed(result.c_str());
	if (!parsed.finish())
		return false;
	if (name == "workspace.execute_batch") {
		acl::json_node *results = json_array_node(parsed["results"]);
		if (results == NULL)
			return false;
		bool retained = true;
		for (acl::json_node *item = results->first_child();
		     item != NULL; item = results->next_child()) {
			acl::json_node *object =
			    item->is_object() ? item : item->get_obj();
			if (object == NULL) {
				retained = false;
				continue;
			}
			const bool saved =
			    remember_read_result(json_text((*object)["name"]),
			        json_text((*object)["result"]), context);
			retained = retained && saved;
		}
		return retained;
	}
	acl::json_node *files = json_array_node(parsed["files"]);
	if (files == NULL)
		return remember_read_page(parsed.get_root(), context);
	bool retained = true;
	for (acl::json_node *item = files->first_child(); item != NULL;
	     item = files->next_child()) {
		acl::json_node *object =
		    item->is_object() ? item : item->get_obj();
		if (!(object == NULL || !remember_read_page(*object, context)))
			continue;
		retained = false;
	}
	return retained;
}

// Check after the whole batch has been inserted: a later page can evict a
// successful earlier put. Only omit raw exchanges when every page still exists.
bool read_result_is_retained(const std::string &name, const std::string &result,
    const webcool::ai::agent_read_context_t &context)
{
	acl::json parsed(result.c_str());
	if (!parsed.finish())
		return false;
	if (name == "workspace.execute_batch") {
		acl::json_node *results = json_array_node(parsed["results"]);
		if (results == NULL)
			return false;
		for (acl::json_node *item = results->first_child();
		     item != NULL; item = results->next_child()) {
			acl::json_node *object =
			    item->is_object() ? item : item->get_obj();
			if (!(object == NULL ||
			        !read_result_is_retained(
			            json_text((*object)["name"]),
			            json_text((*object)["result"]), context)))
				continue;
			return false;
		}
		return true;
	}
	if (name != "workspace.read" && name != "workspace.read_batch")
		return false;
	const auto retained = [&context](acl::json_node &page) {
		return page["content"] != NULL &&
		    context.contains(json_text(page["path"]),
		        json_text(page["file_sha256"]),
		        static_cast<size_t>(json_number(page["offset"], 0)));
	};
	acl::json_node *files = json_array_node(parsed["files"]);
	if (files == NULL)
		return retained(parsed.get_root());
	for (acl::json_node *item = files->first_child(); item != NULL;
	     item = files->next_child()) {
		acl::json_node *object =
		    item->is_object() ? item : item->get_obj();
		if (!(object == NULL || !retained(*object)))
			continue;
		return false;
	}
	return true;
}

static void restore_context_pages(const std::string &transcript, size_t begin,
    size_t end, const std::string &tag,
    webcool::ai::agent_read_context_t &context)
{
	acl::json parsed(
	    transcript.substr(begin + tag.size(), end - begin - tag.size())
	        .c_str());
	if (parsed.finish()) {
		acl::json_node *pages = json_array_node(&parsed.get_root());
		for (acl::json_node *item = pages ? pages->first_child() : NULL;
		     item != NULL; item = pages->next_child()) {
			acl::json_node *object =
			    item->is_object() ? item : item->get_obj();
			if (!(object != NULL))
				continue;
			remember_read_page(*object, context);
		}
	}
}

void restore_read_context(
    const std::string &transcript, webcool::ai::agent_read_context_t &context)
{
	const std::string tag = "<read_working_set>";
	const size_t begin = transcript.rfind(tag);
	size_t offset = 0;
	if (begin != std::string::npos) {
		const size_t end =
		    transcript.find("</read_working_set>", begin);
		if (end != std::string::npos) {
			restore_context_pages(
			    transcript, begin, end, tag, context);
			offset =
			    end + std::string("</read_working_set>").size();
		}
	}
	// Rehydrate reads since the last compaction, including native batch results.
	for (;;) {
		const size_t start = transcript.find("<tool_result>", offset);
		if (start == std::string::npos)
			break;
		const size_t end = transcript.find("</tool_result>", start);
		if (end == std::string::npos)
			break;
		const std::string result =
		    transcript.substr(start + 13, end - start - 13);
		remember_read_result(
		    "workspace.execute_batch", result, context);
		remember_read_result("workspace.read_batch", result, context);
		offset = end + 14;
	}
}

bool project_path_to_draft_path(const std::string &project_path,
    const std::string &path, std::string &draft_path)
{
	if (project_path.empty()) {
		draft_path = path;
		return true;
	}
	if (path == project_path) {
		draft_path.clear();
		return true;
	}
	const std::string prefix = project_path + "/";
	if (path.compare(0, prefix.size(), prefix) != 0)
		return false;
	draft_path = path.substr(prefix.size());
	return true;
}

std::string draft_path_to_project_path(
    const std::string &project_path, const std::string &draft_path)
{
	if (!project_path.empty())
		return draft_path.empty() ? project_path :
		                            project_path + "/" + draft_path;
	return draft_path;
}

// Match the formal source as well as staged changes. External edits must never
// reuse a failure for a different source tree. On an unreadable/large snapshot,
// conservatively disable reuse rather than relying on timestamps alone.
std::string validation_baseline_fingerprint(
    webcool::ai::agent_workspace_t &workspace, const std::string &project_path)
{
	std::string digest, err;
	return workspace.tree_sha256(project_path, digest, err) ? digest : "";
}

void append_edit_snapshot(acl::json &json, acl::json_node &root,
    const std::string &source, size_t at, bool chinese)
{
	const auto excerpt = webcool::ai::edit_excerpt(source, at, 2048);
	acl::json_node &snapshot = json.create_node();
	snapshot.add_text("file_sha256",
	    webcool::ai::agent_workspace_t::content_sha256(source).c_str());
	snapshot.add_number("offset", excerpt.offset);
	snapshot.add_number("total_bytes", source.size());
	snapshot.add_text("content", excerpt.content.c_str());
	snapshot.add_bool("truncated", excerpt.truncated);
	root.add_child("updated_source", snapshot);
	root.add_text(
	    "next_action", prompt_text(prompt_id::edit_snapshot_hint, chinese));
}

static void append_failed_command_summary(
    acl::json &parsed, std::string &summary)
{
	auto *commands = json_array_node(parsed["commands"]);
	for (auto *item = commands ? commands->first_child() : NULL; item;
	     item = commands->next_child()) {
		auto *object = item->is_object() ? item : item->get_obj();
		if (!(object && !json_bool((*object)["passed"], false)))
			continue;
		summary += json_text((*object)["diagnostic"]) +
		    json_text((*object)["error"]);
	}
}

std::string successful_validation_summary(
    const std::string &report, bool chinese)
{
	acl::json parsed(report.c_str());
	const bool parsed_ok = parsed.finish();
	if (parsed_ok && json_bool(parsed["compile_repair_completed"], false)) {
		std::string summary = chinese ?
		    "编译错误修复已通过构建确认" :
		    "Compiler repair confirmed by the build";
		const std::string status = json_text(parsed["test_status"]);
		summary += status == "passed" ?
		    (chinese ? "；改动所在包的测试通过" :
		               "; tests in changed packages passed") :
		    status == "failed" ?
		    (chinese ?
		            "；相关测试仍有问题，已停止自动扩展修复：" :
		            "; related tests still have issues; automatic repair stopped: ") :
		    (chinese ?
		            "；未找到可可靠映射的相关测试，测试未验证" :
		            "; no reliably mapped related tests; tests not verified");
		if (status == "failed") {
			append_failed_command_summary(parsed, summary);
		}
		summary += chinese ?
		    "；未执行全项目测试或功能验收" :
		    "; full-project tests and functional acceptance were not run";
		return summary;
	}
	const bool tests_passed =
	    parsed_ok && json_text(parsed["test_status"]) == "passed";
	return prompt_text(tests_passed ? prompt_id::validation_tests_passed :
	                                  prompt_id::validation_checks_only,
	    chinese);
}

}
}
