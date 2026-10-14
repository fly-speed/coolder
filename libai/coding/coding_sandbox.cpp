#include "coding_internal.h"
#include "coding_records.h"
#include "../sandbox/sandbox_execution.h"
#include "../sdk/file_execution_store.h"
#include "../workspace/agent_workspace.h"
#include "../common/file_ops.h"
#include <map>
#include <mutex>

namespace webcool
{
namespace ai
{
namespace coding_detail
{
#if defined(__unix__) || defined(__APPLE__)
static void link_command_parent(const std::string &,const std::string &,const std::string &,const std::string &);
#endif
namespace
{
std::mutex active_mutex;
std::map<std::string, std::shared_ptr<std::atomic<bool>>> active;

#if defined(__unix__) || defined(__APPLE__)
std::string receipt_json(acl::json_node &node)
{
	const auto &wire=node.to_string();return std::string(wire.c_str(),wire.size());
}
std::string command_scope(const std::string &key)
{
	return "sandbox_command_" + agent_workspace_t::content_sha256(key);
}

void validate_receipt(const std::string &wire, const std::string &key)
{
	libai_schema_json::Document d;
	libai::governance_detail::parse_json_bounded(wire,d,16384);
	if(!d.IsObject() || (d.MemberCount()!=9 && d.MemberCount()!=10) || !d.HasMember("version") ||
	    !d["version"].IsUint() || d["version"].GetUint()!=1 || !d.HasMember("result") || !d["result"].IsObject())
		throw std::invalid_argument("invalid sandbox command receipt");
	for(const auto *name:{"key","plan_id","request_sha256","project_path","command_id","command_sha256","state"}) {
		if(!d.HasMember(name) || !d[name].IsString() ||
		    std::string(d[name].GetString(),d[name].GetStringLength()).find('\0')!=std::string::npos)
			throw std::invalid_argument("invalid sandbox command receipt field");
	}
	const std::string state=d["state"].GetString();
	const bool confirmed=state=="confirmed_none" || state=="confirmed_applied";
	if(std::string(d["key"].GetString())!=key ||
	    (state!="running" && state!="finished" && !confirmed))
		throw std::invalid_argument("sandbox command receipt identity mismatch");
	if(confirmed) {
		if(d.MemberCount()!=10 || !d.HasMember("reference") || !d["reference"].IsString() ||
		    !d["reference"].GetStringLength() || d["reference"].GetStringLength()>2048 ||
		    std::string(d["reference"].GetString(),d["reference"].GetStringLength()).find('\0')!=std::string::npos)
			throw std::invalid_argument("invalid sandbox host reference");
	} else if(d.MemberCount()!=9)
		throw std::invalid_argument("unexpected sandbox host reference");
	auto hex=[](const libai_schema_json::Value &value,size_t length) {
		if(value.GetStringLength()!=length)return false;
		for(size_t i=0;i<length;++i) {char c=value.GetString()[i];if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;}
		return true;
	};
	if(!hex(d["plan_id"],32)||!hex(d["request_sha256"],64)||!hex(d["command_sha256"],64))
		throw std::invalid_argument("invalid sandbox command receipt digest");
	const auto &r=d["result"];
	if(state=="running" || state=="confirmed_none") {
		if(r.MemberCount()!=0)throw std::invalid_argument("unconfirmed sandbox receipt has a result");
		return;
	}
	if(r.MemberCount()!=9)throw std::invalid_argument("invalid sandbox result metadata");
	for(const auto *name:{"broker_ok","started","cancelled","timed_out","output_truncated","error_present"})
		if(!r.HasMember(name)||!r[name].IsBool())throw std::invalid_argument("invalid sandbox result flag");
	if(!r.HasMember("exit_code")||!r["exit_code"].IsInt() || r["exit_code"].GetInt() < -1 || !r.HasMember("elapsed_ms")||!r["elapsed_ms"].IsUint64() ||
	    !r.HasMember("output_sha256")||!r["output_sha256"].IsString()||!hex(r["output_sha256"],64))
		throw std::invalid_argument("invalid sandbox result metadata");
}

std::string command_receipt(const sandbox_execution_plan_t &plan,
    const std::string &helper,const sandbox_result_t *result,bool ok,bool error_present)
{
	acl::json definition;auto &command=definition.create_node();
	command.add_text("id",plan.command.id.c_str()).add_text("executable",plan.command.executable.c_str());
	strings(command,"arguments",plan.command.fixed_arguments);
	command.add_bool("network",plan.command.allow_outbound_network).add_bool("loopback",plan.command.allow_loopback_network)
	    .add_bool("dynamic_arguments",plan.command.allow_dynamic_arguments)
	    .add_number("http_probe_port",plan.command.http_probe_port)
	    .add_text("http_probe_path",plan.command.http_probe_path.c_str())
	    .add_text("browser_node",plan.command.browser_probe_node.c_str())
	    .add_text("browser_evidence_root",plan.command.browser_evidence_root.c_str())
	    .add_number("timeout_ms",static_cast<long long>(plan.limits.timeout_ms))
	    .add_number("memory_bytes",static_cast<long long>(plan.limits.memory_bytes))
	    .add_number("cpu_seconds",plan.limits.cpu_seconds).add_number("process_count",plan.limits.process_count)
	    .add_number("open_files",plan.limits.open_files).add_number("file_size_bytes",static_cast<long long>(plan.limits.file_size_bytes))
	    .add_number("output_bytes",plan.limits.output_bytes);
	acl::json record;auto &r=record.create_node();
	r.add_number("version",1).add_text("key",plan.idempotency_key.c_str()).add_text("plan_id",plan.id.c_str())
	    .add_text("request_sha256",agent_workspace_t::content_sha256(plan.id+"\n"+helper).c_str())
	    .add_text("project_path",plan.project_path.c_str()).add_text("command_id",plan.command.id.c_str())
	    .add_text("command_sha256",agent_workspace_t::content_sha256(receipt_json(command)).c_str())
	    .add_text("state",result?"finished":"running");
	auto &out=record.create_node();r.add_child("result",out);
	if(result) {
		const auto output=std::to_string(result->standard_output.size())+"\n"+result->standard_output+
		    std::to_string(result->standard_error.size())+"\n"+result->standard_error;
		out.add_bool("broker_ok",ok).add_bool("started",result->started).add_bool("cancelled",result->cancelled)
		    .add_bool("timed_out",result->timed_out).add_bool("output_truncated",result->output_truncated)
		    .add_bool("error_present",error_present).add_number("exit_code",result->exit_code)
		    .add_number("elapsed_ms",static_cast<long long>(result->elapsed_ms))
		    .add_text("output_sha256",agent_workspace_t::content_sha256(output).c_str());
	}
	const auto wire=receipt_json(r);validate_receipt(wire,plan.idempotency_key);return wire;
}

bool inspect_command(const std::string &root,acl::json_node &a,acl::json_node &out,std::string &error)
{
	const auto key=text(a,"idempotency_key");
	if(key.empty()||!program_sandbox_t::valid_idempotency_key(key)) {
		error="invalid sandbox inspection key";return false;
	}
	const auto directory=root+"/.webcool_agent/sandbox_commands";
	struct stat info;
	if(stat(directory.c_str(),&info)!=0) {
		if(errno==ENOENT){out.add_bool("found",false);return true;}
		error="cannot inspect sandbox receipt directory";return false;
	}
	libai::file_execution_store_t journal(directory,libai::execution_file_codec_t::plaintext());
	const auto snapshot=journal.open(command_scope(key));
	out.add_bool("found",!snapshot.state_json.empty());
	if(snapshot.state_json.empty())return true;
	validate_receipt(snapshot.state_json,key);
	acl::json receipt(snapshot.state_json.c_str());receipt.finish();
	out.add_text("revision",snapshot.revision.c_str()).add_text("receipt_json",snapshot.state_json.c_str())
	    .add_text("recovery_policy",text(receipt.get_root(),"state")=="running"?"host_reconciliation":"do_not_replay");
	return true;
}

bool confirm_command(const std::string &root,acl::json_node &a,acl::json_node &out,std::string &error)
{
	// Privileged host reconciliation only; never register as a model tool.
	libai_schema_json::Document input;
	libai::governance_detail::parse_json_bounded(receipt_json(a),input,16384);
	const auto key=text(a,"idempotency_key"),expected=text(a,"revision"),outcome=text(a,"outcome"),reference=text(a,"reference");
	if(key.empty() || !program_sandbox_t::valid_idempotency_key(key) || expected.empty() ||
	    (outcome!="none" && outcome!="applied") || reference.empty() || reference.size()>2048 || reference.find('\0')!=std::string::npos ||
	    !input.HasMember("result") || !input["result"].IsObject()) {
		error="invalid sandbox host confirmation";return false;
	}
	libai::file_execution_store_t journal(root+"/.webcool_agent/sandbox_commands",libai::execution_file_codec_t::plaintext());
	const auto scope=command_scope(key);const auto snapshot=journal.open(scope);
	if(snapshot.state_json.empty() || snapshot.revision!=expected) {error="sandbox command receipt revision conflict";return false;}
	validate_receipt(snapshot.state_json,key);
	libai_schema_json::Document record;
	libai::governance_detail::parse_json_bounded(snapshot.state_json,record,16384);
	if(std::string(record["state"].GetString())!="running") {error="sandbox command receipt is already confirmed";return false;}
	auto &allocator=record.GetAllocator();
	record["state"].SetString(outcome=="none"?"confirmed_none":"confirmed_applied",allocator);
	record["result"].CopyFrom(input["result"],allocator);
	libai_schema_json::Value ref;ref.SetString(reference.data(),static_cast<libai_schema_json::SizeType>(reference.size()),allocator);
	record.AddMember("reference",ref,allocator);
	libai_schema_json::StringBuffer buffer;libai_schema_json::Writer<libai_schema_json::StringBuffer> writer(buffer);record.Accept(writer);
	const std::string wire(buffer.GetString(),buffer.GetSize());validate_receipt(wire,key);
	const auto revision=journal.commit(scope,expected,wire);
	out.add_text("revision",revision.c_str()).add_text("state",outcome=="none"?"confirmed_none":"confirmed_applied");
	return true;
}

bool execute(const std::string &root, acl::json_node &a,
    acl::json_node &out, std::string &error)
{
	if (!boolean(a, "confirm", false)) {
		error = "sandbox execution requires explicit confirmation";
		return false;
	}
	const auto supplied_key = text(a, "idempotency_key");
	const auto parent_key = text(a, "parent_idempotency_key");
	if (!program_sandbox_t::valid_idempotency_key(parent_key)) {
		error = "invalid sandbox parent idempotency key"; return false;
	}
	const auto plan_id=text(a,"id");
	if(plan_id.size()!=32 || plan_id.find_first_not_of("0123456789abcdef")!=std::string::npos) {
		error="invalid sandbox plan identity";return false;
	}
	if (!program_sandbox_t::valid_idempotency_key(supplied_key)) {
		error = "invalid sandbox idempotency key";
		return false;
	}
	const auto execution_key=supplied_key.empty()?"sandbox."+plan_id:supplied_key;
	if(!file_ops::make_private_directory(root+"/.webcool_agent") ||
	    !file_ops::make_private_directory(root+"/.webcool_agent/sandbox_commands")) {
		error="cannot prepare private sandbox receipt directory";return false;
	}
	libai::file_execution_store_t journal(root+"/.webcool_agent/sandbox_commands",libai::execution_file_codec_t::plaintext());
	const auto scope=command_scope(execution_key);
	const auto snapshot=journal.open(scope);
	if(!snapshot.state_json.empty()) {
		validate_receipt(snapshot.state_json,execution_key);
		error="sandbox command already has a receipt; inspect before retrying";
		return false;
	}
	sandbox_execution_store_t store(root);
	sandbox_execution_plan_t plan;
	if (!store.consume(text(a, "id"), plan, error))
		return false;
	plan.idempotency_key = execution_key;
	const auto intent=command_receipt(plan,text(a,"helper"),NULL,false,false);
	const auto revision=journal.commit(scope,snapshot.revision,intent);
	link_command_parent(root,plan.project_path,parent_key,execution_key); // Persist ownership before dispatch.
	const auto key = root + ":" + plan.id;
	auto cancel = std::make_shared<std::atomic<bool>>(false);
	{
		std::lock_guard<std::mutex> lock(active_mutex);
		active[key] = cancel;
	}
	struct lease_t {
		std::string key;
		~lease_t()
		{
			std::lock_guard<std::mutex> lock(active_mutex);
			active.erase(key);
		}
	} lease{ key };
	program_sandbox_t sandbox(root, plan.project_path, { plan.command },
	    plan.limits, text(a, "helper"));
	sandbox_request_t request;
	request.command_id = plan.command.id;
	request.idempotency_key = plan.idempotency_key;
	sandbox_result_t result;
	if (!store.audit_started(plan, error))
		return false;
	const bool ok = sandbox.execute(request, result, cancel.get());
	std::string audit_error;
	store.audit_finished(plan, result, audit_error);
	out.add_text("id", plan.id.c_str())
	    .add_text("idempotency_key", plan.idempotency_key.c_str())
	    .add_text("project_path", plan.project_path.c_str())
	    .add_text("command_id", plan.command.id.c_str())
	    .add_bool("started", result.started)
	    .add_bool("cancelled", result.cancelled)
	    .add_bool("timed_out", result.timed_out)
	    .add_bool("output_truncated", result.output_truncated)
	    .add_number("exit_code", result.exit_code)
	    .add_number("elapsed_ms", result.elapsed_ms)
	    .add_text("stdout", result.standard_output.c_str())
	    .add_text("stderr", result.standard_error.c_str());
	error = result.error.empty() ? audit_error : result.error;
	journal.commit(scope,revision,command_receipt(plan,text(a,"helper"),&result,ok,!error.empty()));
	return ok && audit_error.empty();
}
#else
bool execute(const std::string &,acl::json_node &,acl::json_node &,std::string &error)
{
	error="durable sandbox execution requires a supported POSIX file store";return false;
}
bool inspect_command(const std::string &,acl::json_node &,acl::json_node &,std::string &error)
{
	error="sandbox receipt inspection requires a supported POSIX file store";return false;
}
bool confirm_command(const std::string &,acl::json_node &,acl::json_node &,std::string &error)
{
	error="sandbox receipt confirmation requires a supported POSIX file store";return false;
}
#endif
}

#if defined(__unix__) || defined(__APPLE__)
static std::vector<std::string> command_parent_children(const std::string &wire,const std::string &key,const std::string &project)
{
    namespace json=libai::governance_detail::json;
    json::Document record;libai::governance_detail::parse_json_bounded(wire,record,32768);
    if(!record.IsObject()||record.MemberCount()!=4||!record.HasMember("version")||!record["version"].IsInt()||record["version"].GetInt()!=1||
        !record.HasMember("key")||!record["key"].IsString()||std::string(record["key"].GetString(),record["key"].GetStringLength())!=key||
        !record.HasMember("project_path")||!record["project_path"].IsString()||std::string(record["project_path"].GetString(),record["project_path"].GetStringLength())!=project||
        !record.HasMember("children")||!record["children"].IsArray()||record["children"].Size()>64)throw std::invalid_argument("invalid command parent manifest");
    std::vector<std::string> children;std::set<std::string> unique;
    for(const auto &item:record["children"].GetArray()) {
        if(!item.IsString())throw std::invalid_argument("invalid command child key");
        const std::string child(item.GetString(),item.GetStringLength());
        if(child.empty()||!program_sandbox_t::valid_idempotency_key(child)||!unique.insert(child).second)
            throw std::invalid_argument("invalid or duplicate command child key");
        children.push_back(child);
    }
    return children;
}
static void link_command_parent(const std::string &root,const std::string &project,const std::string &parent,const std::string &key)
{
    if(parent.empty())return;
    if(!program_sandbox_t::valid_idempotency_key(parent))throw std::invalid_argument("invalid command parent key");
    libai::file_execution_store_t store(root+"/.webcool_agent/sandbox_commands",libai::execution_file_codec_t::plaintext());
    const auto scope="command_parent_"+agent_workspace_t::content_sha256(parent);const auto snapshot=store.open(scope);
    auto children=snapshot.state_json.empty()?std::vector<std::string>():command_parent_children(snapshot.state_json,parent,project);
    if(std::find(children.begin(),children.end(),key)!=children.end())throw std::runtime_error("command parent already references this key");
    if(children.size()>=64)throw std::runtime_error("command parent child limit reached");children.push_back(key);
    acl::json record;auto &body=record.create_node();body.add_number("version",1).add_text("key",parent.c_str()).add_text("project_path",project.c_str());
    auto &array=record.create_array();body.add_child("children",array);for(const auto &child:children)array.add_array_text(child.c_str());
    store.commit(scope,snapshot.revision,receipt_json(body));
}
#endif
bool inspect_command_parent(const std::string &root,acl::json_node &args,acl::json_node &out,std::string &error)
{
#if defined(__unix__) || defined(__APPLE__)
    try {
        const auto key=text(args,"idempotency_key"),project=text(args,"project_path");
        if(key.empty()||!program_sandbox_t::valid_idempotency_key(key))throw std::invalid_argument("invalid command parent key");
        const auto directory=root+"/.webcool_agent/sandbox_commands";struct stat info;
        if(stat(directory.c_str(),&info)!=0){if(errno==ENOENT){out.add_bool("found",false);return true;}throw std::runtime_error("cannot inspect command parent directory");}
        libai::file_execution_store_t store(directory,libai::execution_file_codec_t::plaintext());
        const auto snapshot=store.open("command_parent_"+agent_workspace_t::content_sha256(key));out.add_bool("found",!snapshot.state_json.empty());
        if(snapshot.state_json.empty())return true;
        const auto children=command_parent_children(snapshot.state_json,key,project);
        out.add_text("revision",snapshot.revision.c_str());coding_detail::strings(out,"children",children);return true;
    }catch(const std::exception &exception){error=exception.what();return false;}
#else
    error="command parent inspection requires POSIX storage";return false;
#endif
}

bool lock_command_children(const std::string &root,const std::string &project,const std::string &parent,const std::string &outcome,
    std::vector<std::shared_ptr<libai::execution_store_t>> &leases,std::string &error)
{
#if defined(__unix__) || defined(__APPLE__)
    try {
        const auto directory=root+"/.webcool_agent/sandbox_commands";struct stat info;
        if(stat(directory.c_str(),&info)!=0){if(errno==ENOENT)return true;throw std::runtime_error("cannot inspect command parents");}
        auto manifest=std::make_shared<libai::file_execution_store_t>(directory,libai::execution_file_codec_t::plaintext());
        const auto snapshot=manifest->open("command_parent_"+agent_workspace_t::content_sha256(parent));leases.push_back(manifest);
        if(snapshot.state_json.empty())return true; // Legacy unlinked calls do not gain fabricated proof.
        for(const auto &key:command_parent_children(snapshot.state_json,parent,project)) {
            auto child=std::make_shared<libai::file_execution_store_t>(directory,libai::execution_file_codec_t::plaintext());
            const auto current=child->open(command_scope(key));leases.push_back(child);
            if(current.state_json.empty())throw std::runtime_error("command child receipt missing: "+key);
            validate_receipt(current.state_json,key);acl::json record(current.state_json.c_str());record.finish();
            const auto state=text(record.get_root(),"state");
            if(text(record.get_root(),"project_path")!=project||state=="running")throw std::runtime_error("command child requires reconciliation: "+key);
            if(outcome=="none"&&state!="confirmed_none")throw std::runtime_error("none parent conflicts with command child effects: "+key);
        }
        return true;
    }catch(const std::exception &exception){error=exception.what();return false;}
#else
    error="command parent checks require POSIX storage";return false;
#endif
}

bool record_command_execution(const std::string &root,const std::string &project,
    const sandbox_command_t &command,const sandbox_limits_t &limits,
    const std::string &key,sandbox_result_t &result,
    const std::function<bool(sandbox_result_t &)> &dispatch,std::string &error,const std::string &parent_key)
{
#if defined(__unix__) || defined(__APPLE__)
	if(key.empty()||!program_sandbox_t::valid_idempotency_key(key)) {
		error="trusted command requires a valid execution key";return false;
	}
	try {
		if(!file_ops::make_private_directory(root+"/.webcool_agent") ||
		    !file_ops::make_private_directory(root+"/.webcool_agent/sandbox_commands")) {
			error="cannot prepare private command receipt directory";return false;
		}
		libai::file_execution_store_t journal(root+"/.webcool_agent/sandbox_commands",libai::execution_file_codec_t::plaintext());
		const auto scope=command_scope(key);const auto snapshot=journal.open(scope);
		if(!snapshot.state_json.empty()) {
			validate_receipt(snapshot.state_json,key);
			error="command already has a receipt; host inspection required";return false;
		}
		sandbox_execution_plan_t plan;plan.id=agent_workspace_t::content_sha256(key).substr(0,32);
		plan.idempotency_key=key;plan.project_path=project;plan.command=command;plan.limits=limits;
		const auto revision=journal.commit(scope,snapshot.revision,command_receipt(plan,"trusted-validator",NULL,false,false));
        link_command_parent(root,project,parent_key,key); // Must commit before dispatch; failure leaves an unconfirmed intent.
		const bool ok=dispatch(result);
		journal.commit(scope,revision,command_receipt(plan,"trusted-validator",&result,ok,!result.error.empty()));
		return ok;
	} catch(const std::exception &failure) {
		error=std::string("command receipt unavailable or outcome unconfirmed: ")+failure.what();
		return false;
	} catch(...) {
		error="command outcome unconfirmed; host inspection required";return false;
	}
#else
	(void)root;(void)project;(void)command;(void)limits;(void)key;(void)result;(void)dispatch;
	error="durable trusted commands require a supported POSIX file store";return false;
#endif
}

bool sandbox(const std::string &root, const std::string &op,
    acl::json_node &a, acl::json_node &out, std::string &error)
{
	if (op == "sandbox.capabilities") {
		std::string reason;
		out.add_bool("available", program_sandbox_t::backend_available(reason))
		    .add_text("reason", reason.c_str());
		return true;
	}
	if (op == "sandbox.execute")
		return execute(root, a, out, error);
	if (op == "sandbox.parent.inspect")
        return inspect_command_parent(root,a,out,error);
	if (op == "sandbox.inspect")
		return inspect_command(root,a,out,error);
	if (op == "sandbox.confirm")
		return confirm_command(root,a,out,error);
	if (op == "sandbox.cancel") {
		std::lock_guard<std::mutex> lock(active_mutex);
		auto found = active.find(root + ":" + text(a, "id"));
		if (found == active.end()) {
			error = "sandbox execution is not active";
			return false;
		}
		found->second->store(true);
		return true;
	}
	return false;
}
}
}
}
