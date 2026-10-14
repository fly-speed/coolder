#include "stdafx.h"
#include "ai_agent_tools_internal.h"
#include "../sdk/file_execution_store.h"
#include "../common/file_ops.h"
#include "../coding/coding_internal.h"

namespace action
{
namespace agent_detail
{

namespace workspace_tool_detail
{

#if defined(__unix__) || defined(__APPLE__)
// Child receipts describe dispatch/return facts, not business acceptance.
class batch_receipts_t {
    struct child_t { std::string key,name,request_sha,state="pending",output_sha; bool ok=false; };
    std::vector<child_t> children_;
    std::unique_ptr<libai::file_execution_store_t> store_;
    std::string key_,project_,scope_,revision_,request_sha_;
    std::mutex mutex_;
    void save() {
        acl::json json; auto &root=json.create_node();
        root.add_number("version",1).add_text("key",key_.c_str()).add_text("project_path",project_.c_str())
            .add_text("request_sha256",request_sha_.c_str());
        auto &children=json.create_array();root.add_child("children",children);
        for (size_t i=0;i<children_.size();++i) {
            const auto &child=children_[i];auto &item=children.add_child(false,true);
            item.add_number("ordinal",static_cast<long long>(i)).add_text("key",child.key.c_str())
                .add_text("name",child.name.c_str()).add_text("request_sha256",child.request_sha.c_str())
                .add_text("state",child.state.c_str()).add_bool("callback_ok",child.ok)
                .add_text("output_sha256",child.output_sha.c_str());
        }
        revision_=store_->commit(scope_,revision_,serialize_json(root));
    }
public:
    batch_receipts_t(const std::string &root,const std::string &project,const std::string &key,
        const agent_tool_request_t &request,const std::vector<agent_tool_request_t> &children)
        :key_(key),project_(project) {
        if(key.empty())return; // Legacy host calls make no durable claim.
        if(!libai::ai::program_sandbox_t::valid_idempotency_key(key))throw std::invalid_argument("invalid batch execution key");
        const auto directory=root+"/.libai_agent/tool_batches";
        if(!libai::ai::file_ops::make_private_directory(root+"/.libai_agent") ||
            !libai::ai::file_ops::make_private_directory(directory))throw std::runtime_error("cannot prepare batch receipts");
        store_.reset(new libai::file_execution_store_t(directory,libai::execution_file_codec_t::plaintext()));
        scope_="tool_batch_"+libai::ai::agent_workspace_t::content_sha256(key);
        const auto snapshot=store_->open(scope_);revision_=snapshot.revision;
        if(!snapshot.state_json.empty())throw std::runtime_error("batch already has child receipts; host inspection required");
        request_sha_=libai::ai::agent_workspace_t::content_sha256(request.content);
        for(size_t i=0;i<children.size();++i) {
            child_t child;child.key="batch."+libai::ai::agent_workspace_t::content_sha256(key+"\n"+std::to_string(i));
            child.name=children[i].name;
            acl::json identity;auto &item=identity.create_node();
            item.add_text("name",child.name.c_str()).add_text("path",children[i].path.c_str())
                .add_text("query",children[i].query.c_str()).add_text("old_text",children[i].old_text.c_str())
                .add_text("target_path",children[i].target_path.c_str()).add_text("content",children[i].content.c_str())
                .add_bool("content_present",children[i].content_present);
            child.request_sha=libai::ai::agent_workspace_t::content_sha256(serialize_json(item));children_.push_back(child);
        }
        save(); // Freeze order and identity before any child or materialization.
    }
    void begin(size_t ordinal) {
        if(!store_)return;std::lock_guard<std::mutex> guard(mutex_);
        auto &child=children_.at(ordinal);
        if(child.state!="pending")throw std::runtime_error("batch child cannot be replayed");
        child.state="running";save();
    }
    void returned(size_t ordinal,const agent_tool_trace_t &trace,const std::string &output) {
        if(!store_)return;std::lock_guard<std::mutex> guard(mutex_);
        auto &child=children_.at(ordinal);child.state="returned";child.ok=trace.ok;
        child.output_sha=libai::ai::agent_workspace_t::content_sha256(output);save();
    }
};
#else
class batch_receipts_t {
public:
    batch_receipts_t(const std::string &,const std::string &,const std::string &key,
        const agent_tool_request_t &,const std::vector<agent_tool_request_t> &) {
        if(!key.empty())throw std::runtime_error("durable batch receipts require POSIX storage");
    }
    void begin(size_t){} void returned(size_t,const agent_tool_trace_t &,const std::string &){}
};
#endif

static std::string parse_workspace_batch(const agent_tool_request_t &request,
    std::vector<agent_tool_request_t> &requests, bool &reads_only)
{
	acl::json batch_json(request.content.c_str());
	acl::json_node *items = batch_json.finish() ?
	    json_array_node(&batch_json.get_root()) :
	    NULL;
	if (items == NULL)
		return "provider tool batch must be a JSON array";
	reads_only = true;
	for (acl::json_node *item = items->first_child(); item != NULL;
	     item = items->next_child()) {
		if (requests.size() >= 16)
			return "provider tool batch contains more than 16 calls";
		acl::json_node *object =
		    item->is_object() ? item : item->get_obj();
		if (object == NULL)
			return "provider tool batch item must be an object";
		agent_tool_request_t child;
		child.name = json_text((*object)["name"]);
		child.path = json_text((*object)["path"]);
		child.query = json_text((*object)["query"]);
		child.old_text = json_text((*object)["old_text"]);
		child.target_path = json_text((*object)["target_path"]);
		child.content_present = (*object)["content"] != NULL &&
		    (*object)["content"]->is_string();
		child.content = child.content_present ?
		    json_text((*object)["content"]) :
		    "";
		if (child.name.empty() ||
		    child.name == "workspace.execute_batch") {
			return "provider tool batch contains an invalid call";
		}
		if (child.name.compare(0, 8, "browser.") == 0)
			return "Browser tools must be called one at a time, outside workspace batches; observe each result before the next action";
		reads_only = reads_only && is_parallel_read_tool(child.name);
		requests.push_back(child);
	}
	if (!requests.empty())
		return "";
	return "provider tool batch requires at least one call";
}

static std::string serialize_workspace_batch(
    const std::vector<agent_tool_request_t> &requests,
    const std::vector<agent_tool_trace_t> &child_traces,
    const std::vector<std::string> &child_results, bool reads_only,
    const std::string &project_path, agent_tool_trace_t &trace)
{
	acl::json response_json;
	acl::json_node &response = response_json.create_node();
	bool all_ok = true;
	for (size_t i = 0; i < child_traces.size(); ++i) {
		all_ok = all_ok && child_traces[i].ok;
	}
	response.add_bool("ok", all_ok);
	response.add_bool("parallel", reads_only);
	acl::json_node &results = response_json.create_array();
	response.add_child("results", results);
	size_t result_bytes = 0;
	for (size_t i = 0; i < requests.size(); ++i) {
		acl::json_node &item = results.add_child(false, true);
		item.add_text("name", requests[i].name.c_str());
		item.add_text("path", child_traces[i].path.c_str());
		item.add_bool("ok", child_traces[i].ok);
		std::string value = child_results[i];
		// Each read result is already bounded by its configured page limit.
		// Preserve its complete JSON, including next_query, in native batches.
		const bool paged_read = requests[i].name == "workspace.read" ||
		    requests[i].name == "workspace.read_batch";
		const size_t remaining = result_bytes < kMaxToolResultBytes ?
		    kMaxToolResultBytes - result_bytes :
		    0;
		if (!paged_read && value.size() > remaining) {
			value = libai::ai::utf8_prefix(value, remaining);
			trace.truncated = true;
		}
		item.add_text("result", value.c_str());
		result_bytes += value.size();
	}
	trace.path = project_path;
	// A parallel batch may return useful results for successful siblings, but
	// the batch itself is successful only when every requested observation was
	// obtained. This keeps retry/no-progress accounting honest.
	trace.ok = all_ok;
	return serialize_json(response);
}

static void cleanup_failed_provider_batch(const std::string &user_root,
    const std::string &project_path, const std::string &batch_run_id)
{
	std::string cleanup_err;
	if (!remove_persistent_worktree(
	        user_root, project_path, batch_run_id, cleanup_err)) {
		libai::ai::ai_log_error("agent.runtime",
		    "cleanup-failed-provider-batch", cleanup_err);
	}
}

// A passing report is reusable only with both its draft fingerprint and its
// source baseline; later batch mutations must not inherit stale success.
static void record_batch_validation(batch_validation_evidence_t &verified,
    const std::string &result, const std::string &baseline_before,
    const std::vector<agent_change_proposal_t> &candidate)
{
	verified = batch_validation_evidence_t();
	acl::json report(result.c_str());
	if (!baseline_before.empty() && report.finish() &&
	    json_bool(report["validation_passed"], false) &&
	    json_number(report["commands_executed"], 0) > 0) {
		verified.report = result;
		verified.draft = staged_cycle_fingerprint(candidate);
		verified.baseline = baseline_before;
	}
}

std::string execute_workspace_batch(libai::ai::agent_workspace_t &workspace,
    const std::string &user_root, const std::string &project_path,
    bool allow_file_content, const agent_tool_request_t &request,
    agent_tool_trace_t &trace,
    std::vector<agent_change_proposal_t> *staged_changes,
    const std::string &run_id,
    const libai::ai::sandbox_limits_t &sandbox_limits, bool chinese,
    const unavailable_validation_t *unavailable_validation,
    validation_cache_t *validation_cache, std::string *saved_proposal_batch,
    batch_validation_evidence_t *batch_validation,
    const std::string &tool_execution_key)
{
    try {
	// This internal request is synthesized only after the provider adapter has
	// authenticated and decoded a multi-call assistant turn. It is not exposed
	// in the model registry. Independent read calls run concurrently in bounded
	// waves; any mutation batch is applied in wire order to a private candidate
	// vector and committed to the caller only when every operation succeeds.
	std::vector<agent_tool_request_t> requests;
	bool reads_only = true;
	const std::string parse_error =
	    parse_workspace_batch(request, requests, reads_only);
	if (!parse_error.empty())
		return tool_error_json(parse_error);

    batch_receipts_t receipts(user_root,project_path,tool_execution_key,request,requests);
	std::vector<agent_tool_trace_t> child_traces(requests.size());
	std::vector<std::string> child_results(requests.size());
	auto child_key = [=](size_t ordinal) {
		return tool_execution_key.empty() ? std::string() :
		    "batch." + libai::ai::agent_workspace_t::content_sha256(
		        tool_execution_key + "\n" + std::to_string(ordinal));
	};
	auto execute_child = [=, &receipts](const agent_tool_request_t child, size_t ordinal)
	    -> std::pair<agent_tool_trace_t, std::string> {
		libai::ai::agent_workspace_t child_workspace(user_root);
		agent_tool_trace_t child_trace;
        receipts.begin(ordinal);
		const std::string result = execute_workspace_tool(
		    child_workspace, user_root, project_path,
		    allow_file_content, child, child_trace, staged_changes,
		    run_id, sandbox_limits, chinese, unavailable_validation,
		    validation_cache, saved_proposal_batch, NULL, child_key(ordinal));
        receipts.returned(ordinal,child_trace,result);
		return std::make_pair(child_trace, result);
	};
	if (reads_only) {
		const size_t concurrency = 4;
		for (size_t begin = 0; begin < requests.size();
		     begin += concurrency) {
			const size_t end =
			    std::min(requests.size(), begin + concurrency);
			std::vector<std::future<
			    std::pair<agent_tool_trace_t, std::string>>>
			    futures;
			for (size_t i = begin; i < end; ++i) {
				const agent_tool_request_t child = requests[i];
				futures.push_back(std::async(
				    std::launch::async, execute_child, child, i));
			}
			for (size_t i = begin; i < end; ++i) {
				const std::pair<agent_tool_trace_t, std::string>
				    value = futures[i - begin].get();
				child_traces[i] = value.first;
				child_results[i] = value.second;
			}
		}
	} else {
		if (staged_changes == NULL)
			return tool_error_json(
			    "provider mutation batch has no review storage");
		std::vector<agent_change_proposal_t> candidate =
		    *staged_changes;
		const std::string batch_run_id = new_run_id();
		if (batch_run_id.empty())
			return tool_error_json(
			    "cannot allocate private provider batch identity");
		size_t skipped_files = 0;
		std::string batch_err;
		if (!materialize_staged_worktree(user_root, project_path,
		        batch_run_id, candidate, skipped_files, batch_err)) {
			libai::ai::ai_log_error("agent.runtime",
			    "initialize-provider-batch", batch_err);
			return tool_error_json(batch_err);
		}
		batch_validation_evidence_t verified;
		for (size_t i = 0; i < requests.size(); ++i) {
			const bool validating =
			    requests[i].name == "workspace.validate";
			const std::string baseline_before = validating ?
			    validation_baseline_fingerprint(
			        workspace, project_path) :
			    "";
            receipts.begin(i);
			child_results[i] =
			    execute_workspace_tool(workspace, user_root,
			        project_path, allow_file_content, requests[i],
			        child_traces[i], &candidate, batch_run_id,
			        sandbox_limits, chinese, unavailable_validation,
				    validation_cache, saved_proposal_batch, NULL, child_key(i));
            receipts.returned(i,child_traces[i],child_results[i]);
			if (!child_traces[i].ok) {
				cleanup_failed_provider_batch(
				    user_root, project_path, batch_run_id);
				acl::json failure_json;
				acl::json_node &failure =
				    failure_json.create_node();
				failure.add_bool("ok", false);
				failure.add_bool("rolled_back", true);
				failure.add_number("failed_call",
				    static_cast<long long>(i + 1));
				failure.add_text("error",
				    ("provider mutation batch failed at call " +
				        std::to_string(i + 1) + ": " +
				        child_results[i])
				        .c_str());
				failure.add_text(
				    "cause", child_results[i].c_str());
				return serialize_json(failure);
			}
			if (validating) {
				record_batch_validation(verified,
				    child_results[i], baseline_before,
				    candidate);
			}
			// Publish this candidate only to the batch-private tree so the next
			// operation observes every earlier edit in the same assistant turn.
			if (materialize_staged_worktree(user_root, project_path,
			        batch_run_id, candidate, skipped_files,
			        batch_err))
				continue;
			libai::ai::ai_log_error("agent.runtime",
			    "advance-provider-batch", batch_err);
			std::string cleanup_err;
			if (remove_persistent_worktree(user_root, project_path,
			        batch_run_id, cleanup_err))
				return tool_error_json(batch_err);
			libai::ai::ai_log_error("agent.runtime",
			    "cleanup-invalid-provider-batch", cleanup_err);

			return tool_error_json(batch_err);
		}
		if (batch_validation && !verified.report.empty() &&
		    verified.draft == staged_cycle_fingerprint(candidate) &&
		    verified.baseline ==
		        validation_baseline_fingerprint(
		            workspace, project_path)) {
			*batch_validation = verified;
		}
		staged_changes->swap(candidate);
		std::string cleanup_err;
		if (!remove_persistent_worktree(
		        user_root, project_path, batch_run_id, cleanup_err)) {
			// The accepted candidate remains valid. A stale private worktree is
			// harmless but must be logged for administrator cleanup.
			libai::ai::ai_log_error("agent.runtime",
			    "cleanup-provider-batch", cleanup_err);
		}
	}

	return serialize_workspace_batch(requests, child_traces, child_results,
	    reads_only, project_path, trace);
    } catch(const std::exception &error) {
        trace.ok=false;return tool_error_json(error.what());
    }
}

} // namespace workspace_tool_detail

#if defined(__unix__) || defined(__APPLE__)
static bool validate_workspace_batch_receipt(const std::string &wire,const std::string &key)
{
        namespace json=libai::governance_detail::json;
        json::Document record;libai::governance_detail::parse_json_bounded(wire,record,32768);
        auto string_field=[](const json::Value &object,const char *name){
            if(!object.HasMember(name)||!object[name].IsString())throw std::invalid_argument("invalid batch receipt string");
            const std::string value(object[name].GetString(),object[name].GetStringLength());
            if(value.find('\0')!=std::string::npos)throw std::invalid_argument("NUL in batch receipt");
            return value;
        };
        auto digest=[](const std::string &value){return value.size()==64 && value.find_first_not_of("0123456789abcdef")==std::string::npos;};
        if(!record.IsObject()||record.MemberCount()!=5||!record.HasMember("version")||!record["version"].IsInt()||record["version"].GetInt()!=1||
            string_field(record,"key")!=key || string_field(record,"project_path").find('\0')!=std::string::npos ||
            !digest(string_field(record,"request_sha256"))||!record.HasMember("children")||!record["children"].IsArray()||
            record["children"].Empty()||record["children"].Size()>16)throw std::invalid_argument("invalid batch receipt identity");
        bool unknown=false;
        for(size_t i=0;i<record["children"].Size();++i) {
            const auto &child=record["children"][static_cast<json::SizeType>(i)];
            if(!child.IsObject()||(child.MemberCount()!=7&&child.MemberCount()!=8)||!child.HasMember("ordinal")||!child["ordinal"].IsUint64()||child["ordinal"].GetUint64()!=i ||
                string_field(child,"key")!="batch."+libai::ai::agent_workspace_t::content_sha256(key+"\n"+std::to_string(i)) ||
                string_field(child,"name").empty()||!digest(string_field(child,"request_sha256"))||
                !child.HasMember("callback_ok")||!child["callback_ok"].IsBool())throw std::invalid_argument("invalid batch child identity");
            const auto state=string_field(child,"state"),output=string_field(child,"output_sha256");
            const bool confirmed=state=="confirmed_none"||state=="confirmed_applied";
            if(confirmed) {
                const auto reference=string_field(child,"reference");
                if(child.MemberCount()!=8||reference.empty()||reference.size()>2048)throw std::invalid_argument("invalid batch host reference");
            } else if(child.MemberCount()!=7)throw std::invalid_argument("unexpected batch host reference");
            if(state=="returned"||state=="confirmed_applied") {if(!digest(output))throw std::invalid_argument("invalid batch result digest");}
            else if(state=="pending"||state=="running"||state=="confirmed_none") {
                if(!output.empty()||child["callback_ok"].GetBool())throw std::invalid_argument("invalid pending batch result");
                unknown=unknown||state=="running";
            } else throw std::invalid_argument("invalid batch child state");
        }
    return unknown;
}
#endif

bool confirm_workspace_batch_child(const std::string &root,const std::string &arguments,acl::json_node &out,std::string &error)
{
#if defined(__unix__) || defined(__APPLE__)
    try {
        namespace json=libai::governance_detail::json;
        json::Document input;libai::governance_detail::parse_json_bounded(arguments,input,16384);
        if(!input.IsObject()||input.MemberCount()!=6||!input.HasMember("child_index")||!input["child_index"].IsUint()||
            !input.HasMember("result")||!input["result"].IsObject())throw std::invalid_argument("invalid batch child confirmation fields");
        auto field=[&](const char *name){if(!input.HasMember(name)||!input[name].IsString())throw std::invalid_argument("invalid batch confirmation string");
            std::string s(input[name].GetString(),input[name].GetStringLength());if(s.find('\0')!=std::string::npos)throw std::invalid_argument("NUL in batch confirmation");return s;};
        const auto key=field("idempotency_key"),revision=field("revision"),outcome=field("outcome"),reference=field("reference");
        if(key.empty()||!libai::ai::program_sandbox_t::valid_idempotency_key(key)||revision.empty()||reference.empty()||reference.size()>2048||
            (outcome!="none"&&outcome!="applied"))throw std::invalid_argument("invalid batch confirmation identity");
        const auto &result=input["result"];
        if(outcome=="none") {if(!result.ObjectEmpty())throw std::invalid_argument("none batch result must be empty");}
        else if(result.MemberCount()!=2||!result.HasMember("output_sha256")||!result["output_sha256"].IsString()||
            result["output_sha256"].GetStringLength()!=64||std::string(result["output_sha256"].GetString()).find_first_not_of("0123456789abcdef")!=std::string::npos||
            !result.HasMember("callback_ok")||!result["callback_ok"].IsBool())throw std::invalid_argument("invalid batch confirmed result");
        libai::file_execution_store_t store(root+"/.libai_agent/tool_batches",libai::execution_file_codec_t::plaintext());
        const auto scope="tool_batch_"+libai::ai::agent_workspace_t::content_sha256(key);const auto snapshot=store.open(scope);
        if(snapshot.state_json.empty()||snapshot.revision!=revision)throw std::runtime_error("batch receipt revision conflict");
        validate_workspace_batch_receipt(snapshot.state_json,key);
        json::Document record;libai::governance_detail::parse_json_bounded(snapshot.state_json,record,32768);
        const auto ordinal=input["child_index"].GetUint();
        if(ordinal>=record["children"].Size())throw std::invalid_argument("batch child index out of range");
        auto &child=record["children"][ordinal];const auto state=std::string(child["state"].GetString());
        if(state!="running"&&state!="returned")throw std::runtime_error("batch child is not awaiting host confirmation");
        std::vector<std::shared_ptr<libai::execution_store_t>> command_leases;
        if(!libai::ai::coding_detail::lock_command_children(root,record["project_path"].GetString(),child["key"].GetString(),outcome,command_leases,error))return false;
        auto &allocator=record.GetAllocator();child["state"].SetString(outcome=="none"?"confirmed_none":"confirmed_applied",allocator);
        child.AddMember("reference",json::Value(reference.c_str(),static_cast<json::SizeType>(reference.size()),allocator),allocator);
        if(outcome=="none"){child["output_sha256"].SetString("",allocator);child["callback_ok"].SetBool(false);}
        else {child["output_sha256"].CopyFrom(result["output_sha256"],allocator);child["callback_ok"].CopyFrom(result["callback_ok"],allocator);}
        json::StringBuffer buffer;json::Writer<json::StringBuffer> writer(buffer);record.Accept(writer);
        const std::string wire(buffer.GetString(),buffer.GetSize());validate_workspace_batch_receipt(wire,key);
        const auto next=store.commit(scope,revision,wire);
        out.add_text("revision",next.c_str()).add_text("state",child["state"].GetString()).add_number("child_index",ordinal);return true;
    }catch(const std::exception &exception){error=exception.what();return false;}
#else
    error="batch confirmation requires POSIX storage";return false;
#endif
}

bool lock_workspace_batch_parent_receipt(const std::string &root,const std::string &project,const std::string &key,
    size_t count,const std::string &outcome,std::vector<std::shared_ptr<libai::execution_store_t>> &leases,std::string &error)
{
#if defined(__unix__) || defined(__APPLE__)
    try {
        const auto directory=root+"/.libai_agent/tool_batches";struct stat info;
        if(stat(directory.c_str(),&info)!=0){if(errno==ENOENT)return true;throw std::runtime_error("cannot inspect batch children");}
        auto lease=std::make_shared<libai::file_execution_store_t>(directory,libai::execution_file_codec_t::plaintext());
        const auto snapshot=lease->open("tool_batch_"+libai::ai::agent_workspace_t::content_sha256(key));leases.push_back(lease);
        if(snapshot.state_json.empty())return true; // Legacy missing child log is not fabricated.
        if(validate_workspace_batch_receipt(snapshot.state_json,key))throw std::runtime_error("batch child requires host reconciliation");
        namespace json=libai::governance_detail::json;json::Document record;
        libai::governance_detail::parse_json_bounded(snapshot.state_json,record,32768);
        if(std::string(record["project_path"].GetString())!=project||record["children"].Size()!=count)
            throw std::runtime_error("batch parent identity or count mismatch");
        for(const auto &child:record["children"].GetArray()) {
            const auto state=std::string(child["state"].GetString()),name=std::string(child["name"].GetString());
            if(!libai::ai::coding_detail::lock_command_children(root,project,child["key"].GetString(),outcome,leases,error))return false;
            if(outcome=="none"&&!is_parallel_read_tool(name)&&state!="pending"&&state!="confirmed_none")
                throw std::runtime_error("none parent receipt conflicts with batch child effects");
        }
        return true;
    }catch(const std::exception &exception){error=exception.what();return false;}
#else
    error="batch parent checks require POSIX storage";return false;
#endif
}

// Trusted host diagnostic; never register as a model tool.
bool inspect_workspace_batch_receipt(const std::string &root,const std::string &key,
    acl::json_node &out,std::string &error)
{
#if defined(__unix__) || defined(__APPLE__)
    try {
        if(key.empty()||!libai::ai::program_sandbox_t::valid_idempotency_key(key))
            throw std::invalid_argument("invalid batch inspection key");
        const auto directory=root+"/.libai_agent/tool_batches";
        struct stat info;
        if(stat(directory.c_str(),&info)!=0) {
            if(errno==ENOENT){out.add_bool("found",false);return true;}
            throw std::runtime_error("cannot inspect batch receipt directory");
        }
        libai::file_execution_store_t store(directory,libai::execution_file_codec_t::plaintext());
        const auto snapshot=store.open("tool_batch_"+libai::ai::agent_workspace_t::content_sha256(key));
        out.add_bool("found",!snapshot.state_json.empty());
        if(snapshot.state_json.empty())return true;
        const bool unknown=validate_workspace_batch_receipt(snapshot.state_json,key);
        out.add_text("revision",snapshot.revision.c_str()).add_text("receipt_json",snapshot.state_json.c_str())
            .add_text("recovery_policy",unknown?"host_reconciliation":"inspect_parent_before_resume");
        return true;
    } catch(const std::exception &exception) {error=exception.what();return false;}
#else
    error="batch inspection requires POSIX storage";return false;
#endif
}

}
}
