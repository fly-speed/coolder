#include "stdafx.h"
// Generated result decisions, per-file reviews and apply HTTP actions.
#include "ai_agent_actions_internal.h"

namespace action
{
using namespace agent_detail;

namespace
{

// One authenticated whole-result decision. The store is opened only after
// resolving the run's project. A false helper result means an error was sent.
struct result_decision_context_t {
	std::string user_root{};
	std::string run_id{};
	std::string decision{};
	bool changes_applied{};
	webcool::ai::agent_run_record_t record{};
	webcool::ai::agent_result_t result{};
	std::string err{};
	bool verified_legacy_apply{};
	std::unique_ptr<webcool::ai::agent_result_store_t> result_store;
};

bool load_result_decision(result_decision_context_t &review, request_t &req,
    response_t &res, operation_trace_t &trace)
{
	if (!current_user_root(req, res, review.user_root))
		return false;
	acl::json *body = req.getJson(64 * 1024);
	if (body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return false;
	}
	review.run_id = json_text((*body)["run_id"]);
	trace.bind(find_runtime_task(review.user_root, review.run_id));
	trace.phase("load_run_and_result");
	review.decision = json_text((*body)["decision"]);
	review.changes_applied = json_bool((*body)["changes_applied"], false);
	webcool::ai::agent_run_store_t run_store(review.user_root);
	if (!load_review_run_record(review.user_root, review.run_id,
	        json_text((*body)["session_id"]), review.record, review.err)) {
		json_error(res, review.err == "agent run not found" ? 404 : 400,
		    review.err.c_str(), req.isKeepAlive());
		return false;
	}
	if (!reviewable_agent_run_status(review.record.status)) {
		json_error(res, 409, "this run result cannot be decided",
		    req.isKeepAlive());
		return false;
	}
	review.result_store.reset(new webcool::ai::agent_result_store_t(
	    review.user_root, review.record.project_path));
	bool found = false;
	if (!review.result_store->load(
	        review.run_id, review.result, found, review.err)) {
		json_error(res, 500, review.err.c_str(), req.isKeepAlive());
		return false;
	}
	trace.phase("validate_review_and_source");
	if (found)
		return true;
	json_error(
	    res, 404, "coding-agent result not found", req.isKeepAlive());
	return false;
}

bool validate_result_decision(
    result_decision_context_t &review, request_t &req, response_t &res)
{
	bool has_pending_generation = false;
	bool has_generation_identity = false;
	for (size_t i = 0; i < review.result.changes.size(); ++i) {
		has_pending_generation = has_pending_generation ||
		    review.result.changes[i].review_status == "pending";
		has_generation_identity = has_generation_identity ||
		    (review.result.changes[i].generation > 0 &&
		        !review.result.changes[i].draft_hash.empty());
	}
	review.verified_legacy_apply = review.decision == "accepted" &&
	    !has_generation_identity && !review.result.changes_applied &&
	    review.changes_applied &&
	    verify_result_changes_applied(
	        review.user_root, review.result, review.err);
	// A partially reviewed modern result may already have changes_applied=true
	// because one accepted file reached disk. It is not proof that the remaining
	// pending generations were written. Finalizing here would mark those files as
	// accepted without applying them, so only the generation-bound review endpoint
	// may resolve them.
	if (review.decision == "accepted" && has_pending_generation &&
	    !review.verified_legacy_apply) {
		review.err =
		    "accept every pending file generation through the review endpoint "
		    "before accepting the whole result";
		webcool::ai::ai_log_error("agent.result",
		    "accept-result-with-pending-generations", review.err);
		json_error(res, 409, review.err.c_str(), req.isKeepAlive());
		return false;
	}
	if (!(review.decision == "accepted" && !review.result.changes_applied &&
	        !review.verified_legacy_apply))
		return true;
	if (review.err.empty()) {
		review.err =
		    "accept file generations through the review endpoint before "
		    "accepting the whole result";
	}
	webcool::ai::ai_log_error(
	    "agent.result", "verify-browser-applied-result", review.err);
	json_error(res, 409, review.err.c_str(), req.isKeepAlive());
	return false;
}

bool persist_result_decision(result_decision_context_t &review, request_t &req,
    response_t &res, operation_trace_t &trace)
{
	trace.phase("persist_review_state");
	if (!review.result_store->set_decision(
	        review.run_id, review.decision, review.result, review.err)) {
		int status =
		    review.err == "coding-agent result not found" ? 404 : 400;
		json_error(res, status, review.err.c_str(), req.isKeepAlive());
		return false;
	}
	// Never trust a browser claim that source bytes were applied. Only the review
	// and result-apply endpoints may set changes_applied, after their server-side
	// compare-and-swap transaction has succeeded.
	if (!(review.verified_legacy_apply && !review.result.changes_applied))
		return true;
	review.result.changes_applied = true;
	review.result.changes_applied_at = static_cast<long long>(time(NULL));
	review.result.changes_apply_error.clear();
	if (review.result_store->save(review.result, review.err))
		return true;
	webcool::ai::ai_log_error(
	    "agent.result", "save-verified-legacy-apply", review.err);
	json_error(res, 500, review.err.c_str(), req.isKeepAlive());
	return false;
}

void publish_result_decision(
    result_decision_context_t &review, operation_trace_t &trace)
{
	trace.phase("publish_review_and_operation_log");
	std::shared_ptr<agent_runtime_task_t> runtime_task =
	    find_runtime_task(review.user_root, review.run_id);
	if (runtime_task) {
		set_runtime_result_artifact(runtime_task,
		    review.result_store->relative_path(review.run_id),
		    review.result.decision);
		if (review.result.changes_applied) {
			// Mirror the server-proven durable state into the cached run snapshot
			// without counting a second filesystem mutation.
			set_runtime_changes_applied(
			    runtime_task, true, "", "", 0);
		}
		acl::json decision_json;
		acl::json_node &decision_event = decision_json.create_node();
		decision_event.add_text("event", "whole_result_reviewed");
		decision_event.add_text(
		    "decision", review.result.decision.c_str());
		decision_event.add_bool(
		    "changes_applied", review.result.changes_applied);
		decision_event.add_number("change_count",
		    static_cast<long long>(review.result.changes.size()));
		append_runtime_operation_event(runtime_task, decision_event);
	}
}

void retire_decided_worktree(
    result_decision_context_t &review, operation_trace_t &trace)
{
	// A whole-result decision resolves every remaining generation. The isolated
	// worktree has served its recovery/review purpose and can now be retired;
	// cleanup failure is operationally visible but must not undo the durable user
	// decision that was already committed above.
	trace.phase("retire_worktree");
	if (review.record.status != "running") {
		std::string cleanup_err;
		if (!schedule_reviewed_worktree_cleanup(review.user_root,
		        review.record.project_path, review.run_id,
		        cleanup_err)) {
			webcool::ai::ai_log_error("agent.draft",
			    "cleanup-decided-worktree", cleanup_err);
		}
	}
}

bool send_result_decision_response(result_decision_context_t &review,
    request_t &req, response_t &res, operation_trace_t &trace)
{
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("run_id", review.run_id.c_str());
	root.add_text("decision", review.result.decision.c_str());
	root.add_bool("changes_applied", review.result.changes_applied);
	root.add_bool("result_persisted", true);
	root.add_text("result_file",
	    review.result_store->relative_path(review.run_id).c_str());
	trace.phase("send_response");
	trace.complete();
	return sendJson(res, 200, root, req.isKeepAlive());
}

} // namespace

bool AiAgentRunResultDecisionAction::run(request_t &req, response_t &res)
{
	operation_trace_t trace("decision");
	result_decision_context_t review;
	if (!load_result_decision(review, req, res, trace))
		return true;
	if (!validate_result_decision(review, req, res))
		return true;
	if (!persist_result_decision(review, req, res, trace))
		return true;
	publish_result_decision(review, trace);
	retire_decided_worktree(review, trace);
	return send_result_decision_response(review, req, res, trace);
}

namespace
{

// State for one authenticated file-review transaction. Validation helpers send
// the error response before returning false; source writes precede review metadata.
struct file_review_context_t {
	std::string user_root{};
	std::string run_id{};
	acl::json *body{};
	std::vector<webcool::ai::agent_change_review_t> reviews{};
	webcool::ai::agent_run_record_t record{};
	webcool::ai::agent_result_t result{};
	std::string err{};
	std::vector<webcool::ai::workspace_change_input_t> accepted_inputs{};
	bool accepted_write_proven{};
	std::unique_ptr<webcool::ai::agent_result_store_t> result_store;
};

long long retire_reviewed_worktree(
    file_review_context_t &context, operation_trace_t &trace)
{
	long long pending = 0;
	for (size_t i = 0; i < context.result.changes.size(); ++i) {
		if (!(context.result.changes[i].review_status == "pending"))
			continue;
		++pending;
	}
	trace.phase("retire_worktree");
	if (!(pending == 0 && context.record.status != "running"))
		return pending;
	std::string cleanup_err;
	if (schedule_reviewed_worktree_cleanup(context.user_root,
	        context.record.project_path, context.run_id, cleanup_err))
		return pending;
	webcool::ai::ai_log_error(
	    "agent.draft", "cleanup-reviewed-worktree", cleanup_err);

	return pending;
}

bool parse_file_review_request(file_review_context_t &context, request_t &req,
    response_t &res, operation_trace_t &trace)
{
	if (!current_user_root(req, res, context.user_root))
		return false;
	context.body = req.getJson(3 * 1024 * 1024);
	if (context.body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return false;
	}
	context.run_id = json_text((*context.body)["run_id"]);
	trace.bind(find_runtime_task(context.user_root, context.run_id));
	trace.phase("load_run_and_result");
	acl::json_node *review_nodes =
	    json_array_node((*context.body)["reviews"]);
	if (review_nodes == NULL) {
		json_error(
		    res, 400, "reviews must be an array", req.isKeepAlive());
		return false;
	}
	for (acl::json_node *item = review_nodes->first_child(); item != NULL;
	     item = review_nodes->next_child()) {
		acl::json_node *object =
		    item->is_object() ? item : item->get_obj();
		if (object == NULL ||
		    context.reviews.size() >= webcool::ai::kMaxAgentChanges) {
			json_error(res, 400, "reviews contains invalid entries",
			    req.isKeepAlive());
			return false;
		}
		webcool::ai::agent_change_review_t review;
		review.path = json_text((*object)["path"]);
		review.generation = static_cast<unsigned long long>(
		    json_number((*object)["generation"], 0));
		review.draft_hash = json_text((*object)["draft_hash"]);
		review.decision = json_text((*object)["decision"]);
		review.resolved_operation =
		    json_text((*object)["resolved_operation"]);
		review.resolved_content =
		    json_text((*object)["resolved_content"]);
		if ((!review.resolved_operation.empty() &&
		        review.resolved_operation != "write" &&
		        review.resolved_operation != "delete") ||
		    review.resolved_content.size() > 1024 * 1024 ||
		    review.resolved_content.find('\0') != std::string::npos) {
			json_error(res, 400,
			    "review contains an invalid resolved file",
			    req.isKeepAlive());
			return false;
		}
		context.reviews.push_back(review);
	}

	return true;
}

bool load_file_review_result(file_review_context_t &context, request_t &req,
    response_t &res, operation_trace_t &trace)
{
	webcool::ai::agent_run_store_t run_store(context.user_root);
	if (!load_review_run_record(context.user_root, context.run_id,
	        json_text((*context.body)["session_id"]), context.record,
	        context.err)) {
		json_error(res,
		    context.err == "agent run not found" ? 404 : 400,
		    context.err.c_str(), req.isKeepAlive());
		return false;
	}
	if (!reviewable_agent_run_status(context.record.status)) {
		json_error(res, 409, "this run result cannot be reviewed",
		    req.isKeepAlive());
		return false;
	}
	context.result_store.reset(new webcool::ai::agent_result_store_t(
	    context.user_root, context.record.project_path));
	bool found = false;
	if (!context.result_store->load(
	        context.run_id, context.result, found, context.err)) {
		json_error(res, 500, context.err.c_str(), req.isKeepAlive());
		return false;
	}
	trace.phase("validate_review_and_source");
	if (found)
		return true;
	json_error(
	    res, 404, "coding-agent result not found", req.isKeepAlive());
	return false;
}

// Match the exact proposal generation before inferring a parent-directory
// dependency; a review of an older draft cannot authorize a newer child edit.
static bool review_requires_directory(const file_review_context_t &context,
    const webcool::ai::agent_change_proposal_t &directory)
{
	bool required = false;
	for (size_t r = 0; r < context.reviews.size() && !required; ++r) {
		if (context.reviews[r].decision != "accepted")
			continue;
		for (size_t p = 0; p < context.result.changes.size(); ++p) {
			const webcool::ai::agent_change_proposal_t &selected =
			    context.result.changes[p];
			if (selected.path != context.reviews[r].path ||
			    selected.generation !=
			        context.reviews[r].generation ||
			    selected.draft_hash !=
			        context.reviews[r].draft_hash)
				continue;
			const std::string target =
			    selected.operation == "move" ?
			    selected.target_path :
			    selected.path;
			required =
			    workspace_path_is_below(target, directory.path);
			break;
		}
	}

	return required;
}

void include_review_directory_dependencies(file_review_context_t &context)
{
	// A file proposed below a new directory depends on that directory proposal.
	// Older browsers and direct API clients may review only the file, so complete
	// the accepted transaction server-side instead of failing with a missing
	// parent path. Directory dependencies are generation-bound just like the file.
	std::vector<webcool::ai::agent_change_review_t> dependency_reviews;
	for (size_t c = 0; c < context.result.changes.size(); ++c) {
		const webcool::ai::agent_change_proposal_t &directory =
		    context.result.changes[c];
		if ((directory.operation != "mkdir" &&
		        directory.operation !=
		            "replace_empty_file_with_directory") ||
		    directory.review_status != "pending")
			continue;
		bool already_reviewed = false;
		for (size_t r = 0; r < context.reviews.size(); ++r) {
			if (!(context.reviews[r].path == directory.path &&
			        context.reviews[r].generation ==
			            directory.generation &&
			        context.reviews[r].draft_hash ==
			            directory.draft_hash))
				continue;
			already_reviewed = true;
			break;
		}
		if (already_reviewed)
			continue;
		const bool required =
		    review_requires_directory(context, directory);
		if (!required)
			continue;
		webcool::ai::agent_change_review_t review;
		review.path = directory.path;
		review.generation = directory.generation;
		review.draft_hash = directory.draft_hash;
		review.decision = "accepted";
		dependency_reviews.push_back(review);
	}
	if (!dependency_reviews.empty()) {
		// Result proposals are dependency ordered, and these directory reviews must
		// be processed before the originally selected child files.
		dependency_reviews.insert(dependency_reviews.end(),
		    context.reviews.begin(), context.reviews.end());
		context.reviews.swap(dependency_reviews);
	}
}

bool prepare_accepted_file_changes(
    file_review_context_t &context, request_t &req, response_t &res)
{
	std::vector<std::string> planned_directories;
	context.accepted_write_proven = false;
	for (size_t i = 0; i < context.reviews.size(); ++i) {
		size_t match = context.result.changes.size();
		for (size_t j = 0; j < context.result.changes.size(); ++j) {
			if (!(context.result.changes[j].path ==
			            context.reviews[i].path &&
			        context.result.changes[j].generation ==
			            context.reviews[i].generation &&
			        context.result.changes[j].draft_hash ==
			            context.reviews[i].draft_hash))
				continue;
			match = j;
			break;
		}
		if (match == context.result.changes.size()) {
			json_error(res, 409,
			    "coding-agent file review generation is stale",
			    req.isKeepAlive());
			return false;
		}
		const webcool::ai::agent_change_proposal_t &change =
		    context.result.changes[match];
		if (change.review_status == "superseded") {
			json_error(res, 409,
			    "coding-agent revision superseded by an accepted continuation; refresh pending reviews",
			    req.isKeepAlive());
			return false;
		}
		if (change.review_status != "pending" &&
		    change.review_status != context.reviews[i].decision) {
			json_error(res, 409,
			    "coding-agent file review was already decided differently",
			    req.isKeepAlive());
			return false;
		}
		if (context.reviews[i].decision != "accepted")
			continue;
		webcool::ai::workspace_change_input_t input;
		input.operation =
		    context.reviews[i].resolved_operation.empty() ?
		    change.operation :
		    context.reviews[i].resolved_operation;
		input.path = change.path;
		input.target_path = change.target_path;
		input.content = context.reviews[i].resolved_operation.empty() ?
		    change.content :
		    context.reviews[i].resolved_content;
		input.reason = change.reason;
		if (change.original_content_available &&
		    (input.operation == "write" ||
		        input.operation == "delete" ||
		        input.operation == "move")) {
			input.enforce_expected_current = true;
			input.expected_current_absent = change.creates_file;
			input.expected_current_content =
			    change.original_content;
		}
		bool already_materialized = false;
		bool below_planned_directory = false;
		for (size_t p = 0; p < planned_directories.size(); ++p) {
			if (!workspace_path_is_below(
			        input.path, planned_directories[p]))
				continue;
			below_planned_directory = true;
			break;
		}
		if (!below_planned_directory &&
		    !workspace_change_matches(context.user_root, input,
		        already_materialized, context.err)) {
			webcool::ai::ai_log_error("agent.result",
			    "inspect-generation-review", context.err);
			json_error(
			    res, 409, context.err.c_str(), req.isKeepAlive());
			return false;
		}
		if (already_materialized) {
			context.accepted_write_proven = true;
			continue;
		}
		if (change.review_status == "accepted") {
			context.err =
			    "accepted coding-agent generation no longer matches formal source";
			webcool::ai::ai_log_error("agent.result",
			    "verify-accepted-generation", context.err);
			json_error(
			    res, 409, context.err.c_str(), req.isKeepAlive());
			return false;
		}
		context.accepted_inputs.push_back(input);
		if (!(input.operation == "mkdir" ||
		        input.operation == "replace_empty_file_with_directory"))
			continue;
		planned_directories.push_back(input.path);
	}

	return true;
}

bool apply_accepted_file_changes(file_review_context_t &context, request_t &req,
    response_t &res, operation_trace_t &trace)
{
	trace.phase("apply_file_transaction");
	if (context.accepted_inputs.empty())
		return true;
	// Acceptance is the write confirmation. Apply all selected generations in
	// one rollback-safe transaction before persisting their accepted state, so
	// the API can never report accepted merely because metadata was updated.
	webcool::ai::workspace_change_set_store_t change_store(
	    context.user_root);
	webcool::ai::workspace_change_set_preview_t preview;
	std::vector<webcool::ai::workspace_change_result_t> applied;
	if (!change_store.create_and_apply(
	        context.accepted_inputs, preview, applied, context.err)) {
		webcool::ai::ai_log_error(
		    "agent.result", "apply-generation-review", context.err);
		json_error(res, 409, context.err.c_str(), req.isKeepAlive());
		return false;
	}
	context.accepted_write_proven = true;

	return true;
}

bool persist_file_review_state(file_review_context_t &context, request_t &req,
    response_t &res, operation_trace_t &trace)
{
	trace.phase("persist_review_state");
	if (!context.result_store->set_change_reviews(
	        context.run_id, context.reviews, context.result, context.err)) {
		const int status =
		    context.err == "coding-agent result not found" ? 404 :
		    context.err.find("stale") != std::string::npos ||
		        context.err.find("already decided") !=
		            std::string::npos ?
		                                                     409 :
		                                                     400;
		json_error(res, status, context.err.c_str(), req.isKeepAlive());
		return false;
	}
	if (!context.accepted_write_proven)
		return true;
	context.result.changes_applied = true;
	context.result.changes_applied_at = static_cast<long long>(time(NULL));
	context.result.changes_apply_error.clear();
	if (context.result_store->save(context.result, context.err))
		return true;
	webcool::ai::ai_log_error(
	    "agent.result", "save-generation-apply-state", context.err);
	json_error(res, 500, context.err.c_str(), req.isKeepAlive());
	return false;
}

void publish_file_review_state(
    file_review_context_t &context, operation_trace_t &trace)
{
	trace.phase("publish_review_and_operation_log");
	std::shared_ptr<agent_runtime_task_t> runtime_task =
	    find_runtime_task(context.user_root, context.run_id);
	if (runtime_task) {
		publish_runtime_change_reviews(
		    runtime_task, context.reviews, context.result);
		acl::json review_json;
		std::vector<acl::json_node *> review_events;
		for (size_t i = 0; i < context.reviews.size(); ++i) {
			acl::json_node &review_event =
			    review_json.create_node();
			review_event.add_text(
			    "event", "file_generation_reviewed");
			review_event.add_text(
			    "path", context.reviews[i].path.c_str());
			review_event.add_number("generation",
			    static_cast<long long>(
			        context.reviews[i].generation));
			review_event.add_text(
			    "decision", context.reviews[i].decision.c_str());
			review_event.add_bool("resolved_content_supplied",
			    !context.reviews[i].resolved_operation.empty());
			if (!context.reviews[i].resolved_content.empty()) {
				review_event.add_number(
				    "resolved_content_bytes",
				    static_cast<long long>(
				        context.reviews[i]
				            .resolved_content.size()));
				review_event.add_text("resolved_content_sha256",
				    webcool::ai::agent_workspace_t::
				        content_sha256(
				            context.reviews[i].resolved_content)
				            .c_str());
			}
			review_events.push_back(&review_event);
		}
		append_runtime_operation_events(runtime_task, review_events);
	}
}

bool send_file_review_response(file_review_context_t &context, request_t &req,
    response_t &res, operation_trace_t &trace)
{
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("run_id", context.run_id.c_str());
	root.add_text("decision", context.result.decision.c_str());
	root.add_bool("changes_applied", context.result.changes_applied);
	const long long pending = retire_reviewed_worktree(context, trace);
	root.add_number("pending_changes", pending);
	trace.phase("send_response");
	trace.complete();
	return sendJson(res, 200, root, req.isKeepAlive());
}

} // namespace

bool AiAgentRunResultReviewAction::run(request_t &req, response_t &res)
{
	operation_trace_t trace("review");
	file_review_context_t context;
	if (!parse_file_review_request(context, req, res, trace))
		return true;
	if (!load_file_review_result(context, req, res, trace))
		return true;
	include_review_directory_dependencies(context);
	if (!prepare_accepted_file_changes(context, req, res))
		return true;
	if (!apply_accepted_file_changes(context, req, res, trace))
		return true;
	if (!persist_file_review_state(context, req, res, trace))
		return true;
	publish_file_review_state(context, trace);
	return send_file_review_response(context, req, res, trace);
}

bool AiAgentRunResultApplyAction::run(request_t &req, response_t &res)
{
	operation_trace_t trace("apply");
	std::string user_root;
	if (!current_user_root(req, res, user_root))
		return true;
	acl::json *body = req.getJson(64 * 1024);
	if (body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return true;
	}
	const std::string run_id = json_text((*body)["run_id"]);
	trace.bind(find_runtime_task(user_root, run_id));
	trace.phase("load_run_and_result");
	webcool::ai::agent_run_store_t run_store(user_root);
	webcool::ai::agent_run_record_t record;
	std::string err;
	if (!run_store.get(run_id, record, err)) {
		json_error(res, err == "agent run not found" ? 404 : 400,
		    err.c_str(), req.isKeepAlive());
		return true;
	}
	if (!reviewable_agent_run_status(record.status)) {
		json_error(res, 409, "only finished run results can be applied",
		    req.isKeepAlive());
		return true;
	}
	webcool::ai::agent_result_store_t result_store(
	    user_root, record.project_path);
	webcool::ai::agent_result_t result;
	bool found = false;
	if (!result_store.load(run_id, result, found, err)) {
		json_error(res, 500, err.c_str(), req.isKeepAlive());
		return true;
	}
	trace.phase("validate_review_and_source");
	if (!found) {
		json_error(res, 404, "coding-agent result not found",
		    req.isKeepAlive());
		return true;
	}
	for (const auto &change : result.changes) {
		if (!(change.review_status == "superseded"))
			continue;
		json_error(res, 409,
		    "coding-agent revision superseded by an accepted continuation; refresh pending reviews",
		    req.isKeepAlive());
		return true;
	}
	// This endpoint is also the crash-recovery path. A completed transaction is
	// idempotent: returning success prevents a reconnecting browser from writing
	// the same result twice.
	if (!result.changes_applied) {
		if (result.decision == "rejected") {
			json_error(res, 409,
			    "rejected coding-agent result cannot be applied",
			    req.isKeepAlive());
			return true;
		}
		if (result.changes.empty()) {
			json_error(res, 409,
			    "coding-agent result contains no file changes",
			    req.isKeepAlive());
			return true;
		}
		std::vector<webcool::ai::workspace_change_input_t> inputs;
		inputs.reserve(result.changes.size());
		for (size_t i = 0; i < result.changes.size(); ++i) {
			webcool::ai::workspace_change_input_t input;
			input.operation = result.changes[i].operation;
			input.path = result.changes[i].path;
			input.target_path = result.changes[i].target_path;
			input.content = result.changes[i].content;
			input.reason = result.changes[i].reason;
			inputs.push_back(input);
		}
		webcool::ai::workspace_change_set_store_t change_store(
		    user_root);
		webcool::ai::workspace_change_set_preview_t preview;
		std::vector<webcool::ai::workspace_change_result_t> applied;
		trace.phase("apply_file_transaction");
		const bool applied_ok = change_store.create_and_apply(
		    inputs, preview, applied, err);
		attach_change_preview_diffs(result.changes, preview);
		if (!applied_ok) {
			result.changes_apply_error = err;
			std::string save_err;
			if (!result_store.save(result, save_err)) {
				webcool::ai::ai_log_error("agent.result",
				    "save-apply-error", save_err);
			}
			webcool::ai::ai_log_error(
			    "agent.result", "recover-auto-apply", err);
			json_error(res, 409, err.c_str(), req.isKeepAlive());
			return true;
		}
		trace.phase("persist_review_state");
		result.changes_applied = true;
		result.changes_applied_at = static_cast<long long>(time(NULL));
		result.changes_apply_error.clear();
		result.decision = "accepted";
		for (size_t i = 0; i < result.changes.size(); ++i) {
			if (!(result.changes[i].review_status == "pending"))
				continue;
			result.changes[i].review_status = "accepted";
		}
		if (!result_store.save(result, err)) {
			webcool::ai::ai_log_error(
			    "agent.result", "save-applied-state", err);
			json_error(res, 500, err.c_str(), req.isKeepAlive());
			return true;
		}
		std::shared_ptr<agent_runtime_task_t> runtime_task =
		    find_runtime_task(user_root, run_id);
		if (runtime_task) {
			const std::string last_path =
			    applied.empty() ? "" : applied.back().path;
			set_runtime_changes_applied(
			    runtime_task, true, "", last_path, applied.size());
		}
	}
	trace.phase("retire_worktree");
	std::string cleanup_err;
	if (!schedule_reviewed_worktree_cleanup(
	        user_root, record.project_path, run_id, cleanup_err)) {
		webcool::ai::ai_log_error(
		    "agent.draft", "cleanup-applied-worktree", cleanup_err);
	}
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("run_id", run_id.c_str());
	root.add_bool("changes_applied", true);
	root.add_number("changes_applied_at", result.changes_applied_at);
	root.add_number(
	    "change_count", static_cast<long long>(result.changes.size()));
	root.add_text(
	    "result_file", result_store.relative_path(run_id).c_str());
	acl::json_node &changes = root.get_json().create_array();
	root.add_child("changes", changes);
	for (size_t i = 0; i < result.changes.size(); ++i) {
		acl::json_node &item = changes.add_child(false, true);
		item.add_text("operation", result.changes[i].operation.c_str());
		item.add_text("path", result.changes[i].path.c_str());
		item.add_text(
		    "target_path", result.changes[i].target_path.c_str());
		item.add_text("content", result.changes[i].content.c_str());
		item.add_text("reason", result.changes[i].reason.c_str());
		item.add_bool("creates_file", result.changes[i].creates_file);
		item.add_bool(
		    "creates_directory", result.changes[i].creates_directory);
		item.add_number("added_lines", result.changes[i].added_lines);
		item.add_number(
		    "removed_lines", result.changes[i].removed_lines);
		item.add_text("diff", result.changes[i].diff.c_str());
		item.add_text("original_content",
		    result.changes[i].original_content.c_str());
		item.add_bool("original_content_available",
		    result.changes[i].original_content_available);
		item.add_number("generation",
		    static_cast<long long>(result.changes[i].generation));
		item.add_text("base_hash", result.changes[i].base_hash.c_str());
		item.add_text(
		    "draft_hash", result.changes[i].draft_hash.c_str());
		item.add_text(
		    "review_status", result.changes[i].review_status.c_str());
	}
	trace.phase("send_response");
	trace.complete();
	return sendJson(res, 200, root, req.isKeepAlive());
}

} // namespace action
