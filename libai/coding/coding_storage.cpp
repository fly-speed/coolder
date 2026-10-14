#include "coding_records.h"
#include "../workspace/agent_workspace.h"
#include "../storage/agent_progress_store.h"
#include "../sdk/file_execution_store.h"
#include "../workspace/agent_review_state.h"
#include "../runtime/coding_runtime.h"
#include "../runtime/browser_debug.h"
#include <algorithm>
#include <set>
#include <limits>
namespace libai
{
namespace ai
{
namespace coding_detail
{
namespace
{
bool inspect_progress(const std::string &root, acl::json_node &a,
    acl::json_node &out, std::string &error)
{
#if defined(__unix__) || defined(__APPLE__)
	const auto project = text(a, "project_path"), session_id = text(a, "session_id");
	if (session_id.empty()) { error = "session_id is required"; return false; }
	agent_session_record_t session;
	if (!agent_session_store_t(root).get(session_id, session, error)) return false;
	if (session.project_path != project) { error = "checkpoint session project mismatch"; return false; }
	std::string absolute;
	if (!agent_workspace_t::resolve_project_state_root(root, project, absolute, error)) return false;
	// Same reserved lease as the real coding loop; no checkpoint mutation or
	// model/tool callback. The file owner releases its lease on every exit.
	libai::file_execution_store_t lease(absolute + "/.webcool_agent",
	    libai::execution_file_codec_t::plaintext());
	const auto snapshot = lease.open("coding_loop_" + session_id);
	if (!snapshot.state_json.empty()) { error = "unexpected coding execution lease state"; return false; }
	agent_progress_t progress; bool found = false; std::string revision;
	if (!agent_progress_store_t(root, project, session_id).load_snapshot(progress, found, revision, error)) return false;
	if (found && (progress.session_id != session_id || progress.project_path != project)) {
		error = "checkpoint identity mismatch"; return false;
	}
	bool unknown = false;
	for (const auto &effect : progress.tool_effects) unknown = unknown || effect.state == "running";
	out.add_number("version", 1).add_bool("has_checkpoint", found)
	    .add_bool("validation_receipt_available",!progress.validation_receipt.effect_key.empty())
	    .add_bool("execution_frozen",!progress.execution_revision.empty())
	    .add_text("execution_revision",progress.execution_revision.c_str())
	    .add_text("checkpoint_revision", revision.c_str())
	    .add_text("recovery_policy", !found ? "new_task" : unknown ? "host_reconciliation" : "resume_checkpoint")
	    .add_number("completed_tool_calls", static_cast<long long>(progress.completed_tool_calls));
	auto &effects = array(out, "effects");
	for (const auto &effect : progress.tool_effects) {
		auto &entry = effects.add_child(false, true);
		entry.add_text("key", effect.key.c_str()).add_text("name", effect.name.c_str())
		    .add_text("state", effect.state.c_str()).add_text("arguments_sha256", effect.arguments_sha256.c_str())
		    .add_text("output_sha256", effect.output_sha256.c_str()).add_text("reference", effect.reference.c_str())
		    .add_number("completed_before", static_cast<long long>(effect.completed_before))
		    .add_number("completed_after", static_cast<long long>(effect.completed_after));
		entry.add_number("tool_call_count",static_cast<long long>(effect.tool_call_count))
		    .add_number("native_call_count",static_cast<long long>(effect.native_call_ids.size()))
		    .add_bool("has_response_continuation",!effect.response_id.empty());
	}
	return true;
#else
	(void)root; (void)a; (void)out;
	error = "progress.inspect requires the POSIX coding execution lease"; return false;
#endif
}
bool sessions(const std::string &root, const std::string &op, acl::json_node &a,
    acl::json_node &out, std::string &error)
{
	agent_session_store_t store(root);
	agent_session_record_t p;
	if (op == "sessions.list") {
		std::vector<agent_session_record_t> entries;
		if (!store.list(limit(a), entries, error))
			return false;
		auto &array = coding_detail::array(out, "sessions");
		for (const auto &entry : entries)
			record(array.add_child(false, true), entry);
		return true;
	}
	if (op == "sessions.remove")
		return store.remove(text(a, "id"), error);
	if (op == "sessions.update")
		return store.update_after_run(text(a, "id"), text(a, "title"),
		    text(a, "summary"), text(a, "last_run_id"),
		    text(a, "user_prompt"), text(a, "assistant_reply"),
		    text(a, "assistant_state"), text(a, "assistant_reasoning"),
		    text(a, "completion_summary"), number(a, "duration_ms"),
		    number(a, "input_tokens"), number(a, "cached_input_tokens"),
		    number(a, "output_tokens"), number(a, "reasoning_tokens"),
		    error);
	bool ok = false;
	if (op == "sessions.get")
		ok = store.get(text(a, "id"), p, error);
	else if (op == "sessions.create") {
		const auto path = text(a, "project_path");
		std::string normalized;
		if (!agent_workspace_t::normalize_path(
		        path, normalized, true, error))
			return false;
		ok = store.create(text(a, "title"), text(a, "agent_id"),
		    text(a, "provider_id"), normalized, p, error);
	}
	if (ok)
		record(out, p);
	return ok;
}
bool runs(const std::string &root, const std::string &op, acl::json_node &a,
    acl::json_node &out, std::string &error)
{
	agent_run_store_t store(root);
	if (op == "runs.list") {
		std::vector<agent_run_record_t> entries;
		if (!store.list(limit(a), entries, error))
			return false;
		auto &array = coding_detail::array(out, "runs");
		for (const auto &entry : entries)
			record(array.add_child(false, true), entry);
		return true;
	}
	if (op == "runs.get") {
		agent_run_record_t p;
		if (!store.get(text(a, "id"), p, error))
			return false;
		record(out, p);
		return true;
	}
	if (op == "runs.audit_cancel")
		return store.cancel(text(a, "id"), error);
	if (op == "runs.fail")
		return store.fail(text(a, "id"), text(a, "error"), error);
	if (op == "runs.complete")
		return store.complete(text(a, "id"), number(a, "input_tokens"),
		    number(a, "cached_input_tokens"),
		    number(a, "output_tokens"), number(a, "reasoning_tokens"),
		    number(a, "latency_ms"), number(a, "tool_calls"),
		    number(a, "proposed_changes"),
		    number(a, "rejected_changes"), error);
	if (op == "runs.create") {
		agent_run_record_t p;
		p.id = text(a, "id");
		p.agent_id = text(a, "agent_id");
		p.agent_version = text(a, "agent_version");
		p.provider_id = text(a, "provider_id");
		p.model = text(a, "model");
		p.project_path = text(a, "project_path");
		p.started_at = number(a, "started_at");
		p.status = "running";
		return store.create(p, error);
	}
	return false;
}
}
bool confirm_progress(const std::string &root, const std::string &arguments,
    acl::json_node &out, std::string &error)
{
#if defined(__unix__) || defined(__APPLE__)
    namespace json=libai::governance_detail::json;
    json::Document input;libai::governance_detail::parse_json_bounded(arguments,input,2*1024*1024);
    if(!input.IsObject() || input.MemberCount()!=8)throw std::invalid_argument("invalid progress.confirm arguments");
    auto string=[](const json::Value &object,const char *key){
        if(!object.HasMember(key)||!object[key].IsString())throw std::invalid_argument("invalid receipt string field");
        std::string value(object[key].GetString(),object[key].GetStringLength());
        if(value.find('\0')!=std::string::npos)throw std::invalid_argument("NUL in receipt field");return value;
    };
    const auto project=string(input,"project_path"),session_id=string(input,"session_id");
    const auto expected=string(input,"checkpoint_revision"),key=string(input,"key");
    const auto outcome=string(input,"outcome"),reference=string(input,"reference");
    std::string output=string(input,"output_json");
    if(session_id.empty() || reference.empty() || reference.size()>2048 ||
        (outcome!="none"&&outcome!="applied") || !input.HasMember("staged_changes") || !input["staged_changes"].IsArray())
        throw std::invalid_argument("invalid receipt identity or outcome");
    agent_session_record_t session;
    if(!agent_session_store_t(root).get(session_id,session,error))return false;
    if(session.project_path!=project){error="checkpoint session project mismatch";return false;}
    std::string absolute;
    if(!agent_workspace_t::resolve_project_state_root(root,project,absolute,error))return false;
    libai::file_execution_store_t loop_lease(absolute+"/.webcool_agent",libai::execution_file_codec_t::plaintext());
    if(!loop_lease.open("coding_loop_"+session_id).state_json.empty()) {error="unexpected coding execution lease state";return false;}
    agent_progress_store_t store(root,project,session_id);agent_progress_t progress;bool found=false;std::string revision;
    if(!store.load_snapshot(progress,found,revision,error))return false;
    if(!found || revision!=expected){error="checkpoint revision conflict";return false;}
    if(progress.tool_effects.empty() || progress.tool_effects.back().key!=key || progress.tool_effects.back().state!="running") {
        error="receipt does not match an unconfirmed tool effect";return false;
    }
    auto &effect=progress.tool_effects.back();
    if(!effect.tool_call_count || effect.completed_before+effect.tool_call_count>256){
        error="receipt requires a recorded tool call count";return false;
    }
    std::vector<std::shared_ptr<libai::execution_store_t>> child_leases;
    if(effect.name.compare(0,8,"browser.")==0 &&
        !browser_debug_lock_parent_receipts(root,project,key,progress.source_run_id,effect.name,outcome,child_leases,error))return false;
    if(effect.name=="workspace.execute_batch" &&
        !action::agent_detail::lock_workspace_batch_parent_receipt(root,project,key,effect.tool_call_count,outcome,child_leases,error))return false;
    if(!lock_command_children(root,project,key,outcome,child_leases,error))return false;
    if(outcome=="none") {
        if(!output.empty() || !input["staged_changes"].Empty())throw std::invalid_argument("none receipt cannot supply output or proposals");
        output=R"({"ok":false,"error":"Trusted host confirms this operation had no effect; any new attempt remains subject to tool policy and budgets."})";
    } else {
        json::Document result;libai::governance_detail::parse_json_bounded(output,result,256*1024);
        if(!result.IsObject())throw std::invalid_argument("receipt output must be a JSON object");
        const auto &items=input["staged_changes"];
        if(items.Size()>256)throw std::invalid_argument("too many receipt proposals");
        std::vector<agent_change_proposal_t> changes;std::set<std::string> paths;
        auto confined=[&](const std::string &path){std::string normalized,err;
            return agent_workspace_t::normalize_path(path,normalized,false,err) && normalized==path && path.compare(0,project.size()+1,project+"/")==0;
        };
        for(const auto &item:items.GetArray()) {
            if(!item.IsObject() || item.MemberCount()!=13)throw std::invalid_argument("invalid receipt proposal fields");
            agent_change_proposal_t change;
            change.operation=string(item,"operation");change.path=string(item,"path");change.target_path=string(item,"target_path");
            change.content=string(item,"content");change.reason=string(item,"reason");change.original_content=string(item,"original_content");
            change.base_hash=string(item,"base_hash");change.draft_hash=string(item,"draft_hash");change.review_status=string(item,"review_status");
            auto boolean=[&](const char *field){if(!item.HasMember(field)||!item[field].IsBool())throw std::invalid_argument("invalid receipt proposal boolean");return item[field].GetBool();};
            change.creates_file=boolean("creates_file");change.creates_directory=boolean("creates_directory");change.original_content_available=boolean("original_content_available");
            if(!item.HasMember("generation")||!item["generation"].IsUint64()||!item["generation"].GetUint64() ||
                item["generation"].GetUint64()>=static_cast<unsigned long long>(std::numeric_limits<long long>::max()))
                throw std::invalid_argument("invalid receipt proposal generation");
            change.generation=item["generation"].GetUint64();
            if(!confined(change.path) || !paths.insert(change.path).second ||
                (change.operation!="write"&&change.operation!="mkdir"&&change.operation!="delete"&&change.operation!="move") ||
                (change.operation=="move" ? !confined(change.target_path) : !change.target_path.empty()) ||
                change.draft_hash!=agent_review_draft_hash(change) ||
                (change.original_content_available && change.base_hash!=agent_workspace_t::content_sha256(change.original_content)))
                throw std::invalid_argument("receipt proposal identity or digest mismatch");
            const agent_change_proposal_t *prior=nullptr;
            for(const auto &old:progress.staged_changes)if(old.path==change.path){prior=&old;break;}
            if(prior && change.generation==prior->generation) {
                if(change.draft_hash!=prior->draft_hash || change.operation!=prior->operation || change.target_path!=prior->target_path ||
                    change.content!=prior->content || change.base_hash!=prior->base_hash || change.original_content!=prior->original_content ||
                    change.original_content_available!=prior->original_content_available || change.review_status!=prior->review_status ||
                    change.creates_file!=prior->creates_file || change.creates_directory!=prior->creates_directory)
                    throw std::invalid_argument("receipt changes an immutable proposal");
            } else if(change.review_status!="pending" || (prior && (change.generation<=prior->generation || change.base_hash!=prior->base_hash ||
                change.original_content!=prior->original_content || change.original_content_available!=prior->original_content_available)))
                throw std::invalid_argument("receipt cannot forge review or replace source baseline");
            changes.push_back(change);
        }
        for(const auto &old:progress.staged_changes)if(!paths.count(old.path))throw std::invalid_argument("receipt drops an existing proposal");
        progress.staged_changes=changes;
    }
    // Preserve native Responses pairing when available; otherwise the trusted
    // result stays in the persisted transcript, without invented provider IDs.
    progress.provider_response_id.clear();progress.provider_response_pending=false;progress.provider_tool_outputs.clear();
    if(!effect.response_id.empty()) {
        std::vector<completion_tool_call_t> calls;
        for(const auto &id:effect.native_call_ids){completion_tool_call_t call;call.id=id;calls.push_back(call);}
        std::vector<completion_tool_output_t> outputs;
        if(!action::agent_detail::build_native_tool_outputs(calls,output,outputs))throw std::invalid_argument("receipt native output batch mismatch");
        for(const auto &value:outputs){
            json::Document checked;libai::governance_detail::parse_json_bounded(value.output,checked,256*1024);
            if(!checked.IsObject())throw std::invalid_argument("receipt native output must be a JSON object");
            agent_progress_tool_output_t saved;saved.call_id=value.call_id;saved.output=value.output;progress.provider_tool_outputs.push_back(saved);
        }
        progress.provider_response_id=effect.response_id;
    }
    acl::json receipt;auto &data=receipt.create_node();
    data.add_text("type","host_tool_receipt").add_text("key",key.c_str()).add_text("outcome",outcome.c_str()).add_text("output_json",output.c_str());
    progress.transcript+="\n"+std::string(data.to_string().c_str())+"\n";
    progress.last_error.clear();
    progress.completed_tool_calls=std::max(progress.completed_tool_calls,effect.completed_before+effect.tool_call_count);
    effect.completed_after=progress.completed_tool_calls;effect.output_sha256=agent_workspace_t::content_sha256(output);
    effect.reference=reference;effect.state=outcome=="none"?"confirmed_none":"confirmed_applied";
    if(outcome=="applied" && !progress.execution_revision.empty() && effect.name.compare(0,7,"coding.")==0 && effect.name.find("_validation")!=std::string::npos) {
        json::Document report;libai::governance_detail::parse_json_bounded(output,report,256*1024);
        auto &cached=progress.validation_receipt;
        cached.effect_key=key;cached.execution_revision=progress.execution_revision;
        cached.source_sha256=string(report,"source_baseline_sha256");cached.proposal_sha256=string(report,"proposal_sha256");cached.report_json=output;
        if(cached.proposal_sha256!=agent_workspace_t::content_sha256(action::agent_detail::staged_change_fingerprint(progress.staged_changes)))
            throw std::invalid_argument("validation receipt does not match confirmed proposals");
    }
    if(!store.save_if_revision(progress,expected,revision,error))return false;
    out.add_number("version",1).add_text("checkpoint_revision",revision.c_str()).add_text("key",key.c_str())
        .add_text("effect_state",effect.state.c_str()).add_text("recovery_policy","resume_checkpoint");
    return true;
#else
    (void)root;(void)arguments;(void)out;error="progress.confirm requires POSIX leases";return false;
#endif
}
bool storage(const std::string &root, const std::string &op, acl::json_node &a,
    acl::json_node &out, std::string &error)
{
	if (op == "progress.inspect") return inspect_progress(root, a, out, error);
	if (op.compare(0, 9, "sessions.") == 0)
		return sessions(root, op, a, out, error);
	if (op.compare(0, 5, "runs.") == 0)
		return runs(root, op, a, out, error);
	sandbox_execution_store_t store(root);
	if (op == "sandbox.history") {
		std::vector<sandbox_run_history_t> entries;
		if (!store.list_history(limit(a), entries, error))
			return false;
		auto &array = coding_detail::array(out, "runs");
		for (const auto &entry : entries)
			record(array.add_child(false, true), entry);
		return true;
	}
	if (op == "sandbox.preview") {
		const auto path = text(a, "project_path");
		std::string absolute;
		if (!agent_workspace_t::resolve_project_root(
		        root, path, absolute, error, false))
			return false;
		sandbox_execution_plan_t p;
		if (!store.create(path, text(a, "command_id"), p, error))
			return false;
		out.add_text("id", p.id.c_str())
		    .add_text("project_path", p.project_path.c_str())
		    .add_text("command_id", p.command.id.c_str())
		    .add_number("timeout_ms", p.limits.timeout_ms)
		    .add_text("executable", p.command.executable.c_str());
		strings(out, "fixed_arguments", p.command.fixed_arguments);
		return true;
	}
	return sandbox(root, op, a, out, error);
}
}
}
}
