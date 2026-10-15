#include <libai/runtime/coding_runtime.h>
#include <libai/common/file_ops.h>
#include <cassert>
#include <fstream>
#include <iterator>
#include <iostream>
#include <cstdlib>
#include <future>
#include <thread>
#include <chrono>
#include <libai/sdk/file_execution_store.h>
#include <libai/coding/coding_service.h>
#include <libai/runtime/coding_tool_loop.h>
#include <libai/runtime/browser_debug.h>
#include <libai/sandbox/sandbox_execution.h>
#include <libai/coding/coding_internal.h>
#include <libai/common/record_codec.h>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>
#endif
using namespace libai::ai;
using namespace action::agent_detail;

namespace action { namespace agent_detail {
// Exercise the production terminal-cleanup boundary with a deterministic
// external broker. Every dispatch inspects the actual persisted checkpoint.
struct coding_loop_test_access_t {
    static void cleanup_receipts(const coding_loop_arguments_t &arguments,
        agent_progress_store_t &store)
    {
        coding_tool_loop_t loop(arguments);
        unsigned dispatches=0;std::string error;
        auto dispatch=[&](bool ok) {
            ++dispatches;
            agent_progress_t saved;bool found=false;
            assert(store.load(saved,found,error)&&found);
            assert(saved.tool_effects.size()==dispatches);
            const auto &intent=saved.tool_effects.back();
            assert(intent.name=="browser.patch_style"&&intent.state=="running");
            assert(intent.tool_call_count==1&&intent.native_call_ids.empty());
            assert(intent.completed_before==dispatches-1&&intent.completed_after==0);
            assert(saved.completed_tool_calls==dispatches-1);
            return ok?std::string("{\"ok\":true,\"undo\":\"all\"}"):
                std::string("{\"ok\":false,\"error\":\"lost acknowledgement\"}");
        };
        assert(loop.cleanup_browser_style([&](const std::string &key){assert(key==arguments.runtime_task->tool_effects.back().key);return dispatch(true);}));
        agent_progress_t saved;bool found=false;
        assert(store.load(saved,found,error)&&found&&saved.completed_tool_calls==1);
        assert(saved.tool_effects.size()==1&&saved.tool_effects[0].state=="done");
        assert(saved.tool_effects[0].reference=="progress:"+saved.tool_effects[0].key);
        assert(saved.transcript.find("undo")!=std::string::npos);
        assert(!loop.cleanup_browser_style([&](const std::string &key){assert(key==arguments.runtime_task->tool_effects.back().key);return dispatch(false);}));
        assert(store.load(saved,found,error)&&found&&saved.tool_effects.size()==2);
        assert(saved.tool_effects.back().state=="running"&&saved.tool_effects.back().reference.empty());
        const auto key=saved.tool_effects.back().key;
        const auto revision=saved.execution_revision;
        assert(!loop.cleanup_browser_style([&](const std::string &){++dispatches;return std::string("{\"ok\":true}");}));
        assert(dispatches==2); // A second cleanup cannot bypass the unknown effect.
        assert(store.load(saved,found,error)&&found&&saved.tool_effects.size()==2);
        assert(saved.tool_effects.back().key==key&&saved.execution_revision==revision);
        // A terminal success must be downgraded when cleanup is unconfirmed.
        // Pair the actual in-process broker, without opening a browser/socket.
        acl::json paired(browser_debug_create(arguments.runtime_task->user_root,
            arguments.project_path,false).c_str());assert(paired.finish());
        const auto token=json_text(paired["token"]);assert(!token.empty());
        const std::string bridge="{\"token\":\""+token+"\",\"client\":\"cleanup-fixture-client\",\"op\":\"";
        acl::json hello((bridge+"hello\",\"url\":\"http://127.0.0.1/fixture\"}").c_str());
        assert(hello.finish());
        acl::json connected(browser_debug_exchange(serialize_json(hello.get_root())).c_str());
        assert(connected.finish()&&json_bool(connected["ok"],false));
        loop.browser_evidence.enabled=true;
        loop.browser_evidence.bind(browser_debug_evidence_id(arguments.runtime_task->user_root,
            arguments.project_path,arguments.runtime_task->id));
        loop.browser_evidence.observe("browser.patch_style","","","temporary-patch","",true);
        assert(!loop.finish_task_acceptance(true));
        acl::json pending(browser_debug_exchange(bridge+"poll\"}").c_str());
        assert(pending.finish()&&json_bool(pending["ok"],false)&&json_text(pending["id"]).empty());
        browser_debug_status(arguments.runtime_task->user_root,arguments.project_path,true);
    }
    static void cleanup_budget(const coding_loop_arguments_t &arguments)
    {
        auto zero=arguments;zero.max_tool_calls=0;
        coding_tool_loop_t loop(zero);unsigned dispatches=0;
        assert(!loop.cleanup_browser_style([&](const std::string &){++dispatches;return std::string("{\"ok\":true}");}));
        assert(dispatches==0&&arguments.runtime_task->tool_effects.empty());
    }
};
}}

static std::string bytes(const std::string &path)
{
    std::ifstream input(path.c_str(),std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
}

#include "workspace_transaction_test.h"

static void browser_execution_keys(const std::string &root,const char *executable)
{
#if defined(__unix__) || defined(__APPLE__)
    const auto policy=ai_runtime_policy_get();auto enabled=policy;enabled.allow_browser_debug=true;
    ai_runtime_policy_set(enabled);
    const std::string owner=root+"/broker-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()),project="project",run=std::string(32,'c');
    assert(file_ops::make_private_directory(owner));
    acl::json paired(browser_debug_create(owner,project,false).c_str());assert(paired.finish());
    const auto token=json_text(paired["token"]);assert(!token.empty());
    auto exchange=[&](const std::string &operation,const std::string &id="",const std::string &result="",const std::string &key="") {
        acl::json request;auto &body=request.create_node();body.add_text("token",token.c_str())
            .add_text("client","browser-key-fixture").add_text("op",operation.c_str());
        if(operation=="hello")body.add_text("url","http://127.0.0.1/fixture");
        if(!id.empty())body.add_text("id",id.c_str());
        if(!result.empty())body.add_text("result",result.c_str());
        if(!key.empty())body.add_text("idempotency_key",key.c_str());
        return browser_debug_exchange(serialize_json(body));
    };
    acl::json hello(exchange("hello").c_str());assert(hello.finish()&&json_bool(hello["ok"],false));
    auto begin=[&](const std::string &key) {
        return std::async(std::launch::async,[&,key](){return browser_debug_tool(owner,project,run,"browser.interact","button","{\"action\":\"click\"}",key);});
    };
    auto poll=[&](const std::string &key) {
        for(int attempt=0;attempt<200;++attempt) {
            acl::json reply(exchange("poll").c_str());assert(reply.finish());
            const auto id=json_text(reply["id"]);
            if(!id.empty()) {
                acl::json command(json_text(reply["command"]).c_str());assert(command.finish());
                assert(json_text(command["idempotency_key"])==key);
                return id;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        assert(false&&"browser command was not dispatched");return std::string();
    };
    const std::string first="browser-fixture.0";auto pending=begin(first);const auto id=poll(first);
    auto raw=[&](const std::string &key){
        const auto path=owner+"/.libai_agent/browser_commands/browser_command_"+agent_workspace_t::content_sha256(key)+".state";
        const auto wire=bytes(path);libai_schema_json::Document envelope;envelope.Parse(wire.c_str());assert(!envelope.HasParseError());
        std::string payload;assert(record_codec::hex_decode(envelope["payload"].GetString(),payload));return payload;
    };
    acl::json dispatched(raw(first).c_str());assert(dispatched.finish()&&json_text(dispatched["state"])=="running");
    assert(json_text(dispatched["command_name"])=="browser.interact"&&json_text(dispatched["output_sha256"]).empty());
    assert(raw(first).find(token)==std::string::npos&&raw(first).find("click")==std::string::npos);
    auto inspect=[&](const std::string &key){return coding_service_call(owner,"browser.receipt.inspect",std::string("{\"idempotency_key\":\"")+key+"\"}");};
    acl::json busy(inspect(first).c_str());assert(busy.finish()&&!json_bool(busy["ok"],false));

    acl::json duplicate(browser_debug_tool(owner,project,run,"browser.interact","button","{\"action\":\"click\"}",first).c_str());
    assert(duplicate.finish()&&!json_bool(duplicate["ok"],true));
    acl::json changed(browser_debug_tool(owner,project,run,"browser.interact","other","{}",first).c_str());
    assert(changed.finish()&&!json_bool(changed["ok"],true));
    acl::json wrong(exchange("result",id,"{\"ok\":true}","different-key").c_str());
    assert(wrong.finish()&&!json_bool(wrong["ok"],true));
    acl::json malformed(exchange("result",id,"{\"ok\":true,\"ok\":false}",first).c_str());
    assert(malformed.finish()&&!json_bool(malformed["ok"],true));
    acl::json reserved(exchange("result",id,"{\"ok\":true,\"browser_execution_reused\":true}",first).c_str());
    assert(reserved.finish()&&!json_bool(reserved["ok"],true));
    acl::json accepted(exchange("result",id,"{\"ok\":true,\"data\":{\"clicked\":true}}",first).c_str());
    assert(accepted.finish()&&json_bool(accepted["ok"],false));
    acl::json result(pending.get().c_str());assert(result.finish()&&json_text(result["idempotency_key"])==first);
    acl::json reused(browser_debug_tool(owner,project,run,"browser.interact","button","{\"action\":\"click\"}",first).c_str());
    assert(reused.finish()&&json_bool(reused["browser_execution_reused"],false));
    assert(json_text(reused["idempotency_key"])==first);
    acl::json persisted(inspect(first).c_str());assert(persisted.finish()&&json_bool(persisted["found"],false));
    acl::json receipt(json_text(persisted["receipt_json"]).c_str());assert(receipt.finish());
    assert(json_text(receipt["state"])=="returned"&&json_bool(receipt["callback_ok"],false)&&json_text(receipt["output_sha256"])==agent_workspace_t::content_sha256(serialize_json(result.get_root())));

    acl::json empty(exchange("poll").c_str());assert(empty.finish()&&json_text(empty["id"]).empty());
#if defined(__unix__) || defined(__APPLE__)
    {
        const auto scope="browser_command_"+agent_workspace_t::content_sha256(first);
        libai::file_execution_store_t store(owner+"/.libai_agent/browser_commands",libai::execution_file_codec_t::plaintext());
        auto snapshot=store.open(scope);
        libai_schema_json::Document forged;forged.Parse(snapshot.state_json.c_str());
        const auto wrong_digest=std::string(64,'0');forged["output_sha256"].SetString(wrong_digest.c_str(),forged.GetAllocator());
        libai_schema_json::StringBuffer buffer;libai_schema_json::Writer<libai_schema_json::StringBuffer> writer(buffer);forged.Accept(writer);
        auto revision=store.commit(scope,snapshot.revision,buffer.GetString());store.close(scope);
        acl::json mismatch(browser_debug_tool(owner,project,run,"browser.interact","button","{\"action\":\"click\"}",first).c_str());
        assert(mismatch.finish()&&!json_bool(mismatch["ok"],true));
        store.open(scope);revision=store.commit(scope,revision,"{}");store.close(scope);
        acl::json corrupt(inspect(first).c_str());assert(corrupt.finish()&&!json_bool(corrupt["ok"],false));
        acl::json refused(browser_debug_tool(owner,project,run,"browser.interact","button","{\"action\":\"click\"}",first).c_str());assert(refused.finish()&&!json_bool(refused["ok"],true));
        store.open(scope);store.commit(scope,revision,snapshot.state_json);store.close(scope);
        acl::json still_empty(exchange("poll").c_str());assert(still_empty.finish()&&json_text(still_empty["id"]).empty());
    }
#endif
    // Confirmed tombstones survive the bounded result cache: never redispatch.
    std::string last;
    for(int index=1;index<=11;++index) {
        last="browser-fixture."+std::to_string(index);auto invocation=begin(last);const auto command=poll(last);
        const auto large="{\"ok\":true,\"data\":\""+std::string(48000,'x')+"\"}";
        acl::json ack(exchange("result",command,large).c_str());assert(ack.finish()&&json_bool(ack["ok"],false));
        acl::json done(invocation.get().c_str());assert(done.finish()&&json_text(done["idempotency_key"])==last);
    }
    acl::json unavailable(browser_debug_tool(owner,project,run,"browser.interact","button","{\"action\":\"click\"}",last).c_str());
    assert(unavailable.finish()&&!json_bool(unavailable["ok"],true));
    acl::json no_replay(exchange("poll").c_str());assert(no_replay.finish()&&json_text(no_replay["id"]).empty());
    auto lost=begin("browser-fixture.disconnect");poll("browser-fixture.disconnect");
    acl::json disconnected(exchange("disconnect").c_str());assert(disconnected.finish()&&json_bool(disconnected["ok"],false));
    acl::json unknown(lost.get().c_str());assert(unknown.finish()&&!json_bool(unknown["ok"],true));
    acl::json retry(browser_debug_tool(owner,project,run,"browser.interact","button","{\"action\":\"click\"}","browser-fixture.disconnect").c_str());
    assert(retry.finish()&&!json_bool(retry["ok"],true));
    const auto lost_key=std::string("browser-fixture.disconnect");
    acl::json lost_record(inspect(lost_key).c_str());assert(lost_record.finish()&&json_text(lost_record["recovery_policy"])=="host_reconciliation");
    const auto lost_wire=raw(lost_key),finished_wire=raw(first);
    browser_debug_status(owner,project,true);
    // A fresh pairing has no process-local cache, but durable keys still block dispatch.
    acl::json fresh(browser_debug_create(owner,project,false).c_str());assert(fresh.finish());
    const auto fresh_token=json_text(fresh["token"]);
    acl::json connect;auto &hello_body=connect.create_node();hello_body.add_text("token",fresh_token.c_str()).add_text("client","fresh-fixture-client").add_text("op","hello").add_text("url","http://127.0.0.1/fixture");
    acl::json reconnect(browser_debug_exchange(serialize_json(hello_body)).c_str());assert(reconnect.finish()&&json_bool(reconnect["ok"],false));
    for(const auto &old_key:{first,lost_key}) {
        acl::json blocked(browser_debug_tool(owner,project,run,"browser.interact","button","{\"action\":\"click\"}",old_key).c_str());
        assert(blocked.finish()&&!json_bool(blocked["ok"],true));
    }
    acl::json poll_body;auto &poll_node=poll_body.create_node();poll_node.add_text("token",fresh_token.c_str()).add_text("client","fresh-fixture-client").add_text("op","poll");
    acl::json nothing(browser_debug_exchange(serialize_json(poll_node)).c_str());assert(nothing.finish()&&json_text(nothing["id"]).empty());
    assert(raw(lost_key)==lost_wire&&raw(first)==finished_wire);
#if defined(__unix__) || defined(__APPLE__)
    for(const auto &old_key:{first,lost_key}) {
        const auto child=fork();assert(child>=0);
        if(child==0){execl(executable,executable,"--browser-replay-probe",owner.c_str(),project.c_str(),old_key.c_str(),static_cast<char *>(NULL));_exit(127);}
        int status=0;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
    }
#endif

    {
        const auto revision=json_text(lost_record["revision"]);
        acl::json args;auto &body=args.create_node();body.add_text("idempotency_key",lost_key.c_str()).add_text("revision",revision.c_str())
            .add_text("outcome","none").add_text("reference","unit-browser-host-reference");
        body.add_child("result",args.create_node());
        const auto wire=serialize_json(body);
        acl::json confirmed(coding_service_call(owner,"browser.receipt.confirm",wire).c_str());
        assert(confirmed.finish()&&json_bool(confirmed["ok"],false)&&json_text(confirmed["state"])=="confirmed_none");
        acl::json duplicate(coding_service_call(owner,"browser.receipt.confirm",wire).c_str());assert(duplicate.finish()&&!json_bool(duplicate["ok"],true));
        acl::json resolved(inspect(lost_key).c_str());assert(resolved.finish()&&json_text(resolved["recovery_policy"])=="inspect_parent_before_resume");
        acl::json blocked(browser_debug_tool(owner,project,run,"browser.interact","button","{\"action\":\"click\"}",lost_key).c_str());
        assert(blocked.finish()&&!json_bool(blocked["ok"],true));
        const auto child=fork();assert(child>=0);
        if(child==0){execl(executable,executable,"--browser-replay-probe",owner.c_str(),project.c_str(),lost_key.c_str(),static_cast<char *>(NULL));_exit(127);}
        int status=0;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
    }
    {
        const auto scope="browser_command_"+agent_workspace_t::content_sha256(first);
        libai::file_execution_store_t store(owner+"/.libai_agent/browser_commands",libai::execution_file_codec_t::plaintext());
        const auto snapshot=store.open(scope);
        libai_schema_json::Document pending;pending.Parse(snapshot.state_json.c_str());
        const std::string output=pending["output_sha256"].GetString();
        pending["state"].SetString("running",pending.GetAllocator());pending["output_sha256"].SetString("",pending.GetAllocator());
        pending["image_sha256"].SetString("",pending.GetAllocator());pending["callback_ok"].SetBool(false);
        libai_schema_json::StringBuffer buffer;libai_schema_json::Writer<libai_schema_json::StringBuffer> writer(buffer);pending.Accept(writer);
        const auto revision=store.commit(scope,snapshot.revision,buffer.GetString());store.close(scope);
        acl::json args;auto &body=args.create_node();body.add_text("idempotency_key",first.c_str()).add_text("revision",revision.c_str())
            .add_text("outcome","applied").add_text("reference","unit-browser-applied-reference");
        auto &result=args.create_node();result.add_text("output_sha256",output.c_str()).add_text("image_sha256","").add_bool("callback_ok",true);body.add_child("result",result);
        acl::json confirmed(coding_service_call(owner,"browser.receipt.confirm",serialize_json(body)).c_str());
        assert(confirmed.finish()&&json_bool(confirmed["ok"],false)&&json_text(confirmed["state"])=="confirmed_applied");
        const auto child=fork();assert(child>=0);
        if(child==0){execl(executable,executable,"--browser-replay-probe",owner.c_str(),project.c_str(),first.c_str(),static_cast<char *>(NULL));_exit(127);}
        int status=0;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
    }
    browser_debug_status(owner,project,true);ai_runtime_policy_set(policy);
#else
    (void)root;(void)executable;
#endif
}

static void trusted_command_receipts(const std::string &root)
{
#if defined(__unix__) || defined(__APPLE__)
    const auto scope_root=root+"/trusted-command-fixture";assert(file_ops::make_private_directory(scope_root));
    sandbox_command_t command;command.id="fixture.command";command.executable="/usr/bin/false";
    sandbox_limits_t limits;sandbox_result_t result;std::string error;unsigned dispatches=0;
    auto inspect=[&](const std::string &key) {
        acl::json args;auto &body=args.create_node();body.add_text("idempotency_key",key.c_str());
        return coding_service_call(scope_root,"sandbox.inspect",serialize_json(body));
    };
    auto raw=[&](const std::string &key) {
        const auto name="sandbox_command_"+agent_workspace_t::content_sha256(key);
        return bytes(scope_root+"/.libai_agent/sandbox_commands/"+name+".state");
    };
    // A successful previous test invocation must not substitute a cached result.
    const auto key="trusted-fixture."+std::to_string(static_cast<long long>(time(NULL)))+"."+std::to_string(getpid());
    const auto parent=key+".parent";
    auto dispatch=[&](sandbox_result_t &value) {
        ++dispatches;acl::json envelope(raw(key).c_str());assert(envelope.finish());
        std::string decoded;assert(record_codec::hex_decode(json_text(envelope["payload"]),decoded));
        acl::json intent(decoded.c_str());assert(intent.finish());
        assert(json_text(intent["state"])=="running"&&json_text(intent["key"])==key);
        acl::json parent_args;auto &body=parent_args.create_node();body.add_text("idempotency_key",parent.c_str()).add_text("project_path","project");
        acl::json linked(coding_service_call(scope_root,"sandbox.parent.inspect",serialize_json(body)).c_str());
        assert(linked.finish()&&json_bool(linked["found"],false));
        auto *children=coding_detail::array_value(linked["children"]);assert(children&&json_text(children->first_child())==key);

        value.started=true;value.exit_code=0;value.standard_output="unit result";return true;
    };
    assert(coding_detail::record_command_execution(scope_root,"project",command,limits,key,result,dispatch,error,parent));
    acl::json finished(inspect(key).c_str());assert(finished.finish()&&json_bool(finished["found"],false));
    assert(json_text(finished["recovery_policy"])=="do_not_replay");
    const auto committed=raw(key);
    {
        std::vector<std::shared_ptr<libai::execution_store_t>> leases;
        assert(coding_detail::lock_command_children(scope_root,"project",parent,"applied",leases,error));
        acl::json busy(inspect(key).c_str());assert(busy.finish()&&!json_bool(busy["ok"],true));
    }
    {
        std::vector<std::shared_ptr<libai::execution_store_t>> leases;
        assert(!coding_detail::lock_command_children(scope_root,"project",parent,"none",leases,error));
    }

    assert(!coding_detail::record_command_execution(scope_root,"project",command,limits,key,result,dispatch,error));
    assert(dispatches==1&&raw(key)==committed);
    const auto unknown=key+".unknown";
    assert(!coding_detail::record_command_execution(scope_root,"project",command,limits,unknown,result,
        [&](sandbox_result_t &)->bool {++dispatches;throw std::runtime_error("lost callback acknowledgement");},error,parent));
    acl::json pending(inspect(unknown).c_str());assert(pending.finish());
    assert(json_text(pending["recovery_policy"])=="host_reconciliation");
    const auto uncertain=raw(unknown);
    {
        std::vector<std::shared_ptr<libai::execution_store_t>> leases;
        assert(!coding_detail::lock_command_children(scope_root,"project",parent,"applied",leases,error));
    }

    assert(!coding_detail::record_command_execution(scope_root,"project",command,limits,unknown,result,dispatch,error));
    assert(dispatches==2&&raw(unknown)==uncertain);
#else
    (void)root;
#endif
}
static void batch_child_receipts(const std::string &root)
{
#if defined(__unix__) || defined(__APPLE__)
    const auto key="unit.batch."+std::to_string(getpid())+"."+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::ofstream(root+"/project/batch-receipt.txt")<<"first observation";
    agent_workspace_t workspace(root);agent_tool_request_t request;request.name="workspace.execute_batch";
    request.content=R"([{"name":"workspace.list","path":"project"},{"name":"workspace.read","path":"project/batch-receipt.txt"}])";
    agent_tool_trace_t trace;sandbox_limits_t limits;
    auto execute=[&]{return execute_workspace_tool(workspace,root,"project",true,request,trace,NULL,"",limits,false,NULL,NULL,NULL,NULL,key);};
    const auto result=execute();assert(trace.ok);
    const auto args=std::string("{\"idempotency_key\":\"")+key+"\"}";
    const auto inspect=coding_service_call(root,"batches.inspect",args);acl::json response(inspect.c_str());
    assert(response.finish()&&json_bool(response["ok"],false)&&json_bool(response["found"],false));
    const auto original=json_text(response["receipt_json"]);acl::json receipt(original.c_str());assert(receipt.finish());
    auto *children=coding_detail::array_value(receipt["children"]);size_t count=0;
    for(auto *child=children->first_child();child;child=children->next_child()) {
        assert(json_text((*child)["state"])=="returned"&&json_bool((*child)["callback_ok"],false));
        assert(json_text((*child)["output_sha256"]).size()==64);++count;
    }
    assert(count==2&&original.find("first observation")==std::string::npos);
    const auto root_key=key+".root";
    execute_workspace_tool(workspace,root,"",true,request,trace,NULL,"",limits,false,NULL,NULL,NULL,NULL,root_key);
    assert(trace.ok);
    acl::json root_inspect(coding_service_call(root,"batches.inspect",std::string("{\"idempotency_key\":\"")+root_key+"\"}").c_str());
    assert(root_inspect.finish()&&json_bool(root_inspect["ok"],false)&&json_bool(root_inspect["found"],false));

    std::ofstream(root+"/project/batch-receipt.txt")<<"changed observation";
    const auto duplicate=execute();assert(!trace.ok&&duplicate.find("already has child receipts")!=std::string::npos);
    acl::json unchanged(coding_service_call(root,"batches.inspect",args).c_str());
    assert(unchanged.finish()&&json_text(unchanged["receipt_json"])==original);
    const auto scope="tool_batch_"+agent_workspace_t::content_sha256(key);
    libai::file_execution_store_t store(root+"/.libai_agent/tool_batches",libai::execution_file_codec_t::plaintext());
    const auto snapshot=store.open(scope);
    acl::json busy(coding_service_call(root,"batches.inspect",args).c_str());assert(busy.finish()&&!json_bool(busy["ok"],false));
    const auto broken=store.commit(scope,snapshot.revision,"{}");store.close(scope);
    acl::json corrupt(coding_service_call(root,"batches.inspect",args).c_str());assert(corrupt.finish()&&!json_bool(corrupt["ok"],false));
    store.open(scope);
    libai_schema_json::Document pending;pending.Parse(original.c_str());assert(!pending.HasParseError());
    auto &first=pending["children"][0];
    first["state"].SetString("running",pending.GetAllocator());first["callback_ok"].SetBool(false);
    first["output_sha256"].SetString("",pending.GetAllocator());
    libai_schema_json::StringBuffer buffer;libai_schema_json::Writer<libai_schema_json::StringBuffer> writer(buffer);pending.Accept(writer);
    const auto pending_revision=store.commit(scope,broken,buffer.GetString());store.close(scope);
    acl::json unknown(coding_service_call(root,"batches.inspect",args).c_str());
    assert(unknown.finish()&&json_bool(unknown["ok"],false)&&json_text(unknown["recovery_policy"])=="host_reconciliation");
    const auto refused=execute();assert(!trace.ok&&refused.find("already has child receipts")!=std::string::npos);
    store.open(scope);store.commit(scope,pending_revision,original);store.close(scope);
#else
    (void)root;
#endif
}

static void result_review_transactions(const std::string &root)
{
#if defined(__unix__) || defined(__APPLE__)
    const auto scope=root+"/review-transactions-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    assert(file_ops::make_private_directory(scope)&&file_ops::make_private_directory(scope+"/project"));
    agent_result_store_t store(scope,"project");agent_result_t original{};
    original.run_id=std::string(32,'a');original.session_id=std::string(32,'b');original.project_path="project";
    original.decision="pending";original.text="worker snapshot";original.saved_at=1;
    for(const auto &name:{"one.txt","two.txt"}) {
        agent_change_proposal_t change{};change.operation="write";change.path=std::string("project/")+name;
        change.content="proposal";change.generation=1;change.draft_hash=agent_workspace_t::content_sha256(change.content);
        change.review_status="pending";change.creates_file=true;original.changes.push_back(change);
    }
    std::string error;assert(store.save(original,error));
    const auto directory=scope+"/project/.libai_agent/results";
    const auto path=scope+"/"+store.relative_path(original.run_id);const auto before=bytes(path);
    agent_change_review_t first;first.path=original.changes[0].path;first.generation=1;first.draft_hash=original.changes[0].draft_hash;first.decision="accepted";
    {
        libai::file_execution_store_t lease(directory,libai::execution_file_codec_t::plaintext());lease.open("result_updates");
        agent_result_t output;
        assert(!store.save(original,error)&&bytes(path)==before);
        assert(!store.set_change_reviews(original.run_id,{first},output,error)&&bytes(path)==before);
    }
    int barrier[2];assert(pipe(barrier)==0);std::vector<pid_t> children;
    for(unsigned ordinal=0;ordinal<2;++ordinal) {
        const auto child=fork();assert(child>=0);
        if(child==0) {
            close(barrier[1]);char signal;assert(read(barrier[0],&signal,1)==1);close(barrier[0]);
            agent_change_review_t review;review.path=original.changes[ordinal].path;review.generation=1;
            review.draft_hash=original.changes[ordinal].draft_hash;review.decision=ordinal==0?"accepted":"rejected";
            agent_result_t output;std::string reason;
            for(unsigned attempt=0;attempt<200;++attempt) {
                if(store.set_change_reviews(original.run_id,{review},output,reason))_exit(0);
                if(reason.find("result update lease unavailable")!=0)_exit(2);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            _exit(3);
        }
        children.push_back(child);
    }
    close(barrier[0]);assert(write(barrier[1],"xx",2)==2);close(barrier[1]);
    for(const auto child:children){int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);}
    agent_result_t reviewed;bool found=false;assert(store.load(original.run_id,reviewed,found,error)&&found);
    assert(reviewed.changes[0].review_status=="accepted"&&reviewed.changes[1].review_status=="rejected");
    assert(store.save(original,error)); // A stale worker must retain both committed decisions.
    assert(store.load(original.run_id,reviewed,found,error)&&found);
    assert(reviewed.changes[0].review_status=="accepted"&&reviewed.changes[1].review_status=="rejected");
    const auto committed=bytes(path);first.decision="rejected";
    assert(!store.set_change_reviews(original.run_id,{first},reviewed,error)&&bytes(path)==committed);
    std::ofstream(path)<<"corrupt";const auto corrupt=bytes(path);
    assert(!store.save(original,error)&&bytes(path)==corrupt);
#else
    (void)root;
#endif
}

int main(int argc,char **argv)
{
    if(argc==5&&std::string(argv[1])=="--browser-replay-probe") {
        auto policy=ai_runtime_policy_get();policy.allow_browser_debug=true;ai_runtime_policy_set(policy);
        acl::json paired(browser_debug_create(argv[2],argv[3],false).c_str());assert(paired.finish());
        const auto token=json_text(paired["token"]);
        acl::json message;auto &hello=message.create_node();hello.add_text("token",token.c_str()).add_text("client","new-process-probe").add_text("op","hello").add_text("url","http://127.0.0.1/fixture");
        acl::json connected(browser_debug_exchange(serialize_json(hello)).c_str());assert(connected.finish()&&json_bool(connected["ok"],false));
        acl::json blocked(browser_debug_tool(argv[2],argv[3],std::string(32,'d'),"browser.interact","button","{\"action\":\"click\"}",argv[4]).c_str());
        assert(blocked.finish()&&!json_bool(blocked["ok"],true)&&json_text(blocked["error"]).find("persistent receipt")!=std::string::npos);
        acl::json request;auto &poll=request.create_node();poll.add_text("token",token.c_str()).add_text("client","new-process-probe").add_text("op","poll");
        acl::json none(browser_debug_exchange(serialize_json(poll)).c_str());assert(none.finish()&&json_text(none["id"]).empty());
        return 0;
    }
    assert(argc==2);
    {
        agent_tool_request_t batch; batch.name="workspace.execute_batch";
        batch.content=R"([{"name":"workspace.patch_set"},{"name":"workspace.replace"}])";
        assert(!coding_tool_requires_confirmation(batch));
        batch.content=R"([{"name":"workspace.list"},{"name":"workspace.read"}])";
        assert(!coding_tool_requires_confirmation(batch));
        batch.content=R"([{"name":"workspace.patch_set"},{"name":"workspace.validate"}])";
        assert(coding_tool_requires_confirmation(batch));
        batch.content=R"([{"name":"workspace.patch_set"},{"name":"browser.interact"}])";
        assert(coding_tool_requires_confirmation(batch));
        batch.content=R"([{"name":"unknown.command","side_effect_free":true}])";
        assert(coding_tool_requires_confirmation(batch));
        batch.content="[]";
        assert(coding_tool_requires_confirmation(batch));
    }
    // A concurrent review cannot grant permission to replay external effects.
    // Mixed batches retain uncertainty even when their private draft rolls back.
    for(const auto &name:std::vector<std::string>{"browser.interact","browser.patch_style",
        "browser.snapshot","workspace.validate","workspace.execute_batch"}) {
        agent_tool_request_t request;request.name=name;
        request.content=R"({"rolled_back":true,"side_effect_free":true})";
        assert(!may_retry_coding_tool_after_review(request));
        assert(coding_tool_requires_confirmation(request));
    }
    for(const auto &name:std::vector<std::string>{"workspace.propose","workspace.propose_batch",
        "workspace.patch_set","workspace.replace","workspace.propose_delete",
        "workspace.propose_move","workspace.propose_mkdir"}) {
        agent_tool_request_t request;request.name=name;
        assert(may_retry_coding_tool_after_review(request));
        assert(!coding_tool_requires_confirmation(request));
    }
    for(const auto &name:std::vector<std::string>{"workspace.read","workspace.create",
        "workspace.mkdir","workspace.propose_custom","custom.business.write",""}) {
        agent_tool_request_t request;request.name=name;
        assert(!may_retry_coding_tool_after_review(request));
    }
    for(const auto &wire:std::vector<std::string>{
        R"({"type":"final","text":"Done","changes":[{"path":"one.txt","content":"valid"},{"path":"two.txt","content":42}]})",
        R"({"type":"final","text":"Done","changes":[{"path":"one.txt","content":"first","content":"second"}]})",
        R"({"type":"final","text":"Done","changes":[{"path":"one.txt","content":"a\u0000b"}]})",
        R"({"type":"final","text":"Done","changes":[{"path":"one.txt","content":"valid","review_status":"accepted"}]})",
        R"({"type":"final","text":"Done","changes":[{"path":"one.txt"}]})",
        R"({"type":"final","text":42,"changes":[]})",
        R"({"type":"final","text":"Done","changes":{}})"}) {
        agent_protocol_message_t parsed;
        assert(!validate_agent_final_message(wire).empty()&&!parse_agent_protocol_message(wire,parsed));
        assert(parsed.changes.empty());
    }
    {
        std::string excessive=R"({"type":"final","text":"Done","changes":[)";
        for(int index=0;index<21;++index){if(index)excessive+=",";excessive+=R"({"path":"file.txt","content":"valid"})";}
        excessive+="]}";assert(!validate_agent_final_message(excessive).empty());
        agent_protocol_message_t parsed;
        const auto valid=R"({"type":"final","text":"Done","changes":[{"path":"one.txt","content":""},{"operation":"delete","path":"two.txt"},{"operation":"move","path":"three.txt","target_path":"four.txt"}]})";
        assert(validate_agent_final_message(valid).empty()&&parse_agent_protocol_message(valid,parsed)&&parsed.changes.size()==3);
        assert(parse_agent_protocol_message("```json\n"+std::string(valid)+"\n```",parsed));
    }
    const std::string root=argv[1], project="project", prompt="Inspect this project.";
    assert(file_ops::make_private_directory(root));
    assert(file_ops::make_private_directory(root+"/"+project));
    browser_execution_keys(root,argv[0]);
    trusted_command_receipts(root);
    result_review_transactions(root);
    workspace_transaction_leases(root);
    batch_child_receipts(root);
    {
        assert(program_sandbox_t::valid_idempotency_key("agent-scope.0:compile"));
        for(const auto &bad:std::vector<std::string>{std::string(257,'a'),"a\nb","a=b","a/b",std::string("a\0b",3)})
            assert(!program_sandbox_t::valid_idempotency_key(bad));
        sandbox_execution_store_t audit(root);sandbox_execution_plan_t plan;
        plan.id=std::string(32,'f');plan.project_path=project;plan.command.id="fixture.key";
        plan.idempotency_key="agent-scope.0:compile";std::string audit_error;
        assert(audit.audit_started(plan,audit_error));
        std::vector<sandbox_run_history_t> history;
        assert(audit.list_history(10,history,audit_error)&&history.size()==1);
        assert(history[0].idempotency_key==plan.idempotency_key&&history[0].status=="interrupted");
        sandbox_result_t outcome;outcome.exit_code=0;
        assert(audit.audit_finished(plan,outcome,audit_error));
        assert(audit.list_history(10,history,audit_error)&&history[0].idempotency_key==plan.idempotency_key);
        assert(history[0].status=="completed");
        const auto legacy_root=root+"/legacy-key-audit";
        assert(file_ops::make_private_directory(legacy_root));
        assert(file_ops::make_private_directory(legacy_root+"/.libai_agent"));
        const auto legacy_file=legacy_root+"/.libai_agent/sandbox_runs.v1";
        std::ofstream(legacy_file.c_str()) <<
            "{\"version\":1,\"format\":\"LIBAI_SANDBOX_AUDIT_V1\",\"records\":["
            "{\"fields\":[\"1\",\"started\",\"dddddddddddddddddddddddddddddddd\",\"70726f6a656374\",\"66697874757265\",\"-1\",\"0\",\"0\",\"0\",\"0\",\"\"]},"
            "{\"fields\":[\"2\",\"finished\",\"dddddddddddddddddddddddddddddddd\",\"70726f6a656374\",\"66697874757265\",\"0\",\"0\",\"1\",\"0\",\"0\",\"\",\"0\"]}]}";
        assert(sandbox_execution_store_t(legacy_root).list_history(10,history,audit_error)&&history.size()==1);
        assert(history[0].idempotency_key.empty()&&history[0].status=="completed");
#if defined(__APPLE__) || defined(__linux__)
        if(const auto *helper=std::getenv("LIBAI_TEST_SANDBOX_HELPER")) {
            sandbox_command_t command;command.id="fixture.key";command.executable="/usr/bin/printenv";
            command.fixed_arguments={"LIBAI_IDEMPOTENCY_KEY"};
            program_sandbox_t sandbox(root,project,{command},sandbox_limits_t(),helper);
            sandbox_request_t request;request.command_id=command.id;request.idempotency_key=plan.idempotency_key;
            sandbox_result_t result;
            assert(sandbox.execute(request,result)&&result.started&&result.exit_code==0);
            assert(result.standard_output==plan.idempotency_key+"\n");
            request.idempotency_key="malicious\nENV=value";
            assert(!sandbox.execute(request,result)&&!result.started);
        }
#endif
    }
    provider_config_t provider;provider.id="fixture";provider.model="fixture";
    provider.protocol="openai_responses";provider.base_url="http://127.0.0.1:1/v1";
    provider.response_state_mode="provider";
    agent_session_record_t session;std::string session_error;
    assert(agent_session_store_t(root).create("Recovery fixture","coding",provider.id,project,session,session_error));
    const std::string session_id=session.id;
    {
        agent_session_record_t cleanup_session;
        assert(agent_session_store_t(root).create("Cleanup fixture","coding",provider.id,project,cleanup_session,session_error));
        auto task=std::make_shared<agent_runtime_task_t>();task->id=std::string(32,'b');
        task->user_root=root;task->session_id=cleanup_session.id;task->project_path=project;
        task->original_prompt=prompt;
        agent_workspace_t workspace(root);completion_result_t output;
        std::vector<agent_tool_trace_t> traces;std::vector<agent_change_proposal_t> changes;
        std::vector<completion_image_t> images;std::string key,transcript,reasoning,thinking,effort,memory,summary,title,error;
        sandbox_limits_t limits;size_t rejected=0;
        coding_loop_arguments_t arguments{provider,key,workspace,project,prompt,transcript,reasoning,images,
            0,4,1024,4,65536,limits,thinking,effort,output,traces,changes,memory,summary,title,rejected,task,error};
        agent_progress_store_t cleanup_store(root,project,cleanup_session.id);
        auto original_policy=ai_runtime_policy_get();auto browser_policy=original_policy;
        browser_policy.allow_browser_debug=true;ai_runtime_policy_set(browser_policy);
        coding_loop_test_access_t::cleanup_budget(arguments);
        error.clear();
        coding_loop_test_access_t::cleanup_receipts(arguments,cleanup_store);
        assert(error.find("unconfirmed tool effect")!=std::string::npos);
        assert(cleanup_store.remove(error));
        ai_runtime_policy_set(original_policy);
    }
#if defined(__unix__) || defined(__APPLE__)
    const std::string inspect_args="{\"project_path\":\"project\",\"session_id\":\""+session_id+"\"}";
    auto inspect=[&](bool expected_ok,const std::string &policy){
        const auto reply=coding_service_call(root,"progress.inspect",inspect_args);
        acl::json parsed(reply.c_str());assert(parsed.finish());
        assert(json_bool(parsed["ok"],false)==expected_ok);
        if(expected_ok)assert(json_text(parsed["recovery_policy"])==policy);
        assert(reply.find("Original frozen context.")==std::string::npos);
        assert(reply.find("Inspect this project.")==std::string::npos);
        return reply;
    };
#endif
    agent_progress_store_t store(root,project,session_id);
    agent_progress_t progress;progress.provider_id=provider.id;progress.provider_model=provider.model;
    progress.session_id=session_id;progress.project_path=project;progress.original_prompt=prompt;progress.transcript="Original frozen context.";
    progress.provider_response_id="resp_original";progress.provider_response_pending=true;
    progress.updated_at=1;
    auto invoke=[&](bool resumed,size_t cursor,const std::string &expected_response,const std::string &expected_error,bool pending_response=true,bool retain_worker_lease=false){
        std::string error;const std::string before=bytes(root+"/"+store.relative_path());
        auto task=std::make_shared<agent_runtime_task_t>();task->id=std::string(32,'a');task->user_root=root;
        task->session_id=session_id;task->project_path=project;task->original_prompt=prompt;task->resumed_from_progress=resumed;
        // Stop before any provider request/tool call, after real startup recovery.
        task->cancel_requested=true;
        agent_workspace_t workspace(root);completion_result_t result;
        std::vector<agent_tool_trace_t> traces;std::vector<agent_change_proposal_t> changes;
        std::vector<completion_image_t> images;std::string memory,summary,title;
        sandbox_limits_t limits;size_t rejected=0;
        std::unique_ptr<coding_worker_lease_guard_t> worker;
        if(retain_worker_lease) {
            worker.reset(new coding_worker_lease_guard_t(task));
            bool rejected_duplicate=false;
            try {coding_worker_lease_guard_t duplicate(task);}
            catch(const std::logic_error &){rejected_duplicate=true;}
            assert(rejected_duplicate&&task->retain_coding_worker_lease);
        }
        bool ok=run_coding_tool_loop(provider,"",workspace,project,prompt,progress.transcript,"",images,
            cursor,4,1024,4,64*1024,limits,"","",result,traces,changes,memory,summary,title,rejected,task,error);
        if(ok || error.find(expected_error)==std::string::npos) std::cerr << "resumed=" << resumed << " cursor=" << cursor << " expected=" << expected_error << " actual=" << error << "\n";
        assert(!ok&&error.find(expected_error)!=std::string::npos);
#if defined(__unix__) || defined(__APPLE__)
        if(retain_worker_lease) {
            assert(task->retain_coding_worker_lease&&task->coding_worker_lease);
            const auto child=fork();assert(child>=0);
            if(child==0) {
                try {
                    libai::file_execution_store_t other(root+"/project/.libai_agent",libai::execution_file_codec_t::plaintext());
                    other.open("coding_loop_"+task->session_id);_exit(1);
                } catch(const std::exception &) {_exit(0);}
            }
            int status=0;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
            worker.reset();assert(!task->retain_coding_worker_lease&&!task->coding_worker_lease);
            libai::file_execution_store_t released(root+"/project/.libai_agent",libai::execution_file_codec_t::plaintext());
            const auto scope="coding_loop_"+task->session_id;assert(released.open(scope).state_json.empty());released.close(scope);
        }
#endif

        assert(task->provider_response_id==expected_response);
        assert(task->provider_response_pending==(pending_response&&!expected_response.empty()));
        assert(traces.empty()&&changes.empty());
        if(expected_error=="cancelled") {
            // Normal startup commits refreshed continuation metadata. It must
            // retain the recovered response rather than create a fresh chain.
            agent_progress_t saved;bool found=false;std::string load_error;
            assert(store.load(saved,found,load_error)&&found);
            assert(saved.provider_response_id==expected_response);
            assert(saved.provider_response_pending==(pending_response&&!expected_response.empty()));
            assert(saved.completed_tool_calls==cursor);
        } else assert(bytes(root+"/"+store.relative_path())==before);
    };
    std::string error;assert(store.save(progress,error));
#if defined(__unix__) || defined(__APPLE__)
    {
        const auto path=root+"/"+store.relative_path();
        const auto original_bytes=bytes(path);
        std::string revision;bool found=false;agent_progress_t loaded;
        assert(store.load_snapshot(loaded,found,revision,error)&&found);
        assert(revision==agent_workspace_t::content_sha256(original_bytes)&&revision.size()==64);
        { std::ofstream append(path.c_str(),std::ios::binary|std::ios::app);append << " "; }
        std::string whitespace_revision;
        assert(store.load_snapshot(loaded,found,whitespace_revision,error)&&found&&whitespace_revision!=revision);
        assert(whitespace_revision==agent_workspace_t::content_sha256(bytes(path)));
        assert(store.save(progress,error));
        auto changed_progress=progress;changed_progress.transcript+=" Changed.";
        assert(store.save(changed_progress,error));
        std::string changed_revision;
        assert(store.load_snapshot(loaded,found,changed_revision,error)&&found&&changed_revision!=revision);
        assert(store.save(progress,error));
        auto refused=[&](){
            revision="stale";found=true;
            assert(!store.load_snapshot(loaded,found,revision,error)&&!found&&revision.empty());
            inspect(false,"");
        };
        assert(::chmod(path.c_str(),0644)==0);refused();assert(::chmod(path.c_str(),0600)==0);
        assert(::link(path.c_str(),(path+".linked").c_str())==0);refused();assert(::unlink((path+".linked").c_str())==0);
        assert(::rename(path.c_str(),(path+".saved").c_str())==0);
        assert(::symlink((path+".saved").c_str(),path.c_str())==0);refused();assert(::unlink(path.c_str())==0);
        // O_NONBLOCK ensures an invalid FIFO cannot hang recovery.
        assert(::mkfifo(path.c_str(),0600)==0);refused();assert(::unlink(path.c_str())==0);
        assert(::mkdir(path.c_str(),0700)==0);refused();assert(::rmdir(path.c_str())==0);
        assert(::rename((path+".saved").c_str(),path.c_str())==0);
        const auto directory=root+"/"+project+"/.libai_agent";
        assert(::chmod(directory.c_str(),0755)==0);refused();assert(::chmod(directory.c_str(),0700)==0);
        assert(bytes(path)==original_bytes);
        const auto reply=inspect(true,"resume_checkpoint");
        assert(reply.find(agent_workspace_t::content_sha256(original_bytes))!=std::string::npos);
        assert(store.load_snapshot(loaded,found,revision,error)&&found);
        auto candidate=progress;candidate.transcript+=" CAS committed.";
        std::string committed;
        assert(store.save_if_revision(candidate,revision,committed,error)&&committed.size()==64&&committed!=revision);
        const auto committed_bytes=bytes(path);
        assert(committed==agent_workspace_t::content_sha256(committed_bytes));
        std::string refused_revision="stale";
        assert(!store.save_if_revision(progress,revision,refused_revision,error)&&refused_revision.empty());
        assert(error=="checkpoint revision conflict"&&bytes(path)==committed_bytes);
        assert(!store.save_if_revision(progress,"invalid",refused_revision,error)&&bytes(path)==committed_bytes);
        assert(store.save(progress,error));
        assert(store.load_snapshot(loaded,found,revision,error)&&found);
        // Independent processes compete for the same snapshot. Only one may
        // replace it, including normal writes/removal under the record lease.
        int results[2],gate[2];assert(pipe(results)==0&&pipe(gate)==0);
        pid_t contenders[2];
        for(int index=0;index<2;++index){
            contenders[index]=fork();assert(contenders[index]>=0);
            if(!contenders[index]){
                close(results[0]);close(gate[1]);char signal=0;assert(read(gate[0],&signal,1)==1);
                auto next=progress;next.transcript+=" contender "+std::to_string(index);
                std::string result,error;const bool ok=store.save_if_revision(next,revision,result,error);
                const char winner=ok?'1':'0';assert(write(results[1],&winner,1)==1);_exit(0);
            }
        }
        close(results[1]);close(gate[0]);assert(write(gate[1],"xx",2)==2);
        unsigned wins=0;for(int index=0;index<2;++index){char winner=0;assert(read(results[0],&winner,1)==1);wins+=winner=='1';}
        assert(wins==1);
        for(auto pid:contenders){int status=0;assert(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0);}
        close(results[0]);close(gate[1]);
        assert(store.load_snapshot(loaded,found,committed,error)&&found&&committed!=revision);
        assert(loaded.transcript==progress.transcript+" contender 0"||loaded.transcript==progress.transcript+" contender 1");
        assert(store.save(progress,error));
    }
    int ready[2],release[2];assert(pipe(ready)==0&&pipe(release)==0);
    const auto child=fork();assert(child>=0);
    if(child==0){
        close(ready[0]);close(release[1]);
        libai::file_execution_store_t owner(root+"/"+project+"/.libai_agent",libai::execution_file_codec_t::plaintext());
        owner.open("coding_progress_"+session_id);
        assert(write(ready[1],"r",1)==1);char signal=0;assert(read(release[0],&signal,1)==1);
        // Kernel release after an ungraceful exit, without owner.close().
        _exit(0);
    }
    close(ready[1]);close(release[0]);char signal=0;assert(read(ready[0],&signal,1)==1);
    const auto locked_bytes=bytes(root+"/"+store.relative_path());
    std::string refused_revision;
    assert(!store.save(progress,error));assert(!store.remove(error));
    assert(!store.save_if_revision(progress,agent_workspace_t::content_sha256(locked_bytes),refused_revision,error));
    assert(refused_revision.empty()&&bytes(root+"/"+store.relative_path())==locked_bytes);
    // Reads can observe the previous committed snapshot while a writer owns
    // its separate record lease; the existing loop lease still gates inspect.
    inspect(true,"resume_checkpoint");
    assert(write(release[1],"x",1)==1);int child_status=0;assert(waitpid(child,&child_status,0)==child);
    assert(WIFEXITED(child_status)&&WEXITSTATUS(child_status)==0);close(ready[0]);close(release[1]);
    assert(pipe(ready)==0&&pipe(release)==0);const auto loop_child=fork();assert(loop_child>=0);
    if(loop_child==0){
        close(ready[0]);close(release[1]);
        libai::file_execution_store_t owner(root+"/"+project+"/.libai_agent",libai::execution_file_codec_t::plaintext());
        owner.open("coding_loop_"+session_id);assert(write(ready[1],"r",1)==1);
        char signal=0;assert(read(release[0],&signal,1)==1);_exit(0);
    }
    close(ready[1]);close(release[0]);assert(read(ready[0],&signal,1)==1);
    inspect(false,"");invoke(true,0,"","coding execution lease unavailable");
    assert(write(release[1],"x",1)==1);assert(waitpid(loop_child,&child_status,0)==loop_child);
    assert(WIFEXITED(child_status)&&WEXITSTATUS(child_status)==0);close(ready[0]);close(release[1]);
#endif
#if defined(__unix__) || defined(__APPLE__)
    inspect(true,"resume_checkpoint");
    for(const auto &args : std::vector<std::string>{
        "{\"project_path\":\"project\",\"session_id\":\""+session_id+"\",\"session_id\":\"other\"}",
        "{\"project_path\":\"other\",\"session_id\":\""+session_id+"\"}",
        "{\"project_path\":\"project\",\"session_id\":\""+session_id+"\",\"scope\":\"other\"}"}) {
        const auto saved=bytes(root+"/"+store.relative_path());
        acl::json denied(coding_service_call(root,"progress.inspect",args).c_str());
        assert(denied.finish()&&!json_bool(denied["ok"],false));
        assert(bytes(root+"/"+store.relative_path())==saved);
    }
#endif
    invoke(true,0,"resp_original","cancelled");
    {
        agent_progress_t saved;bool found=false;assert(store.load(saved,found,error)&&found);
        assert(saved.execution_revision.size()==64);
        progress.execution_revision=saved.execution_revision;assert(store.save(progress,error));
        const auto policy=ai_runtime_policy_get();auto changed_policy=policy;changed_policy.allow_build_network=!policy.allow_build_network;
        ai_runtime_policy_set(changed_policy);
        invoke(true,0,"","execution configuration changed");
        ai_runtime_policy_set(policy);
        invoke(true,0,"resp_original","cancelled");
        progress.execution_revision=std::string(64,'b');assert(store.save(progress,error));
        invoke(true,0,"","execution configuration changed");
        progress.execution_revision.clear();assert(store.save(progress,error));
    }
    invoke(false,0,"","cancelled");
    agent_tool_effect_t pending;pending.key=std::string(32,'c')+".0";pending.name="workspace.propose";
    pending.arguments_sha256=std::string(64,'a');pending.state="running";
    pending.tool_call_count=2;pending.native_call_ids={"call-one","call-two"};pending.response_id="resp_pending";
    progress.tool_effects.push_back(pending);assert(store.save(progress,error));
    {
        auto fixture=progress;fixture.execution_revision=std::string(64,'d');fixture.completed_tool_calls=2;
        auto &effect=fixture.tool_effects.back();effect.name="coding.final_validation";effect.state="done";
        effect.completed_after=2;effect.reference="progress:"+effect.key;
        auto &receipt=fixture.validation_receipt;receipt.effect_key=effect.key;receipt.execution_revision=fixture.execution_revision;
        receipt.source_sha256=std::string(64,'a');receipt.proposal_sha256=std::string(64,'c');
        receipt.report_json="{\"ok\":true,\"source_baseline_sha256\":\""+receipt.source_sha256+"\",\"proposal_sha256\":\""+receipt.proposal_sha256+"\",\"validation_passed\":false}";
        effect.output_sha256=agent_workspace_t::content_sha256(receipt.report_json);
        assert(store.save(fixture,error));agent_progress_t loaded;bool found=false;
        assert(store.load(loaded,found,error)&&found&&loaded.validation_receipt.report_json==receipt.report_json);
        const auto checkpoint=bytes(root+"/"+store.relative_path());
        auto invalid=fixture;invalid.validation_receipt.execution_revision=std::string(64,'e');assert(!store.save(invalid,error));
        invalid=fixture;invalid.validation_receipt.effect_key="missing";assert(!store.save(invalid,error));
        invalid=fixture;invalid.validation_receipt.report_json="{}";assert(!store.save(invalid,error));
        invalid=fixture;invalid.tool_effects.back().state="running";invalid.tool_effects.back().reference.clear();invalid.tool_effects.back().completed_after=0;assert(!store.save(invalid,error));
        assert(bytes(root+"/"+store.relative_path())==checkpoint);assert(store.save(progress,error));
    }
#if defined(__unix__) || defined(__APPLE__)
    {
        agent_progress_t loaded;bool found=false;std::string revision;
        assert(store.load_snapshot(loaded,found,revision,error)&&found);
        assert(loaded.tool_effects.back().native_call_ids==pending.native_call_ids);
        assert(loaded.tool_effects.back().response_id==pending.response_id);
        assert(loaded.tool_effects.back().tool_call_count==2);
        const auto path=root+"/"+store.relative_path(),saved_bytes=bytes(path);
        auto invalid=progress;invalid.tool_effects.back().native_call_ids={"same","same"};
        assert(!store.save(invalid,error)&&bytes(path)==saved_bytes);
        invalid=progress;invalid.tool_effects.back().native_call_ids={"only-one"};
        assert(!store.save(invalid,error)&&bytes(path)==saved_bytes);
        invalid=progress;invalid.tool_effects.back().native_call_ids.clear();
        assert(!store.save(invalid,error)&&bytes(path)==saved_bytes);
        // Original eight-field checkpoints remain valid, without guessing IDs.
        libai_schema_json::Document legacy;
        libai::governance_detail::parse_json_bounded(saved_bytes,legacy,4*1024*1024);
        auto &effect=legacy["tool_effects"][0];
        effect.RemoveMember("tool_call_count");effect.RemoveMember("native_call_ids");effect.RemoveMember("response_id");
        libai_schema_json::StringBuffer buffer;libai_schema_json::Writer<libai_schema_json::StringBuffer> writer(buffer);
        legacy.Accept(writer);
        {std::ofstream old(path.c_str(),std::ios::binary|std::ios::trunc);old.write(buffer.GetString(),buffer.GetSize());}
        assert(store.load_snapshot(loaded,found,revision,error)&&found);
        assert(loaded.tool_effects.back().native_call_ids.empty()&&loaded.tool_effects.back().response_id.empty());
        assert(loaded.tool_effects.back().tool_call_count==0);
        assert(store.save(progress,error));
    }
    const auto inspection_before=bytes(root+"/"+store.relative_path());
    auto cancelled_task=std::make_shared<agent_runtime_task_t>();cancelled_task->id=std::string(32,'f');
    cancelled_task->user_root=root;cancelled_task->session_id=session_id;
    const auto cancel_note=preserve_cancelled_coding_progress(cancelled_task,project);
    assert(cancel_note.find(pending.key)!=std::string::npos&&cancelled_task->recovery_available);
    assert(cancelled_task->progress_file==store.relative_path()&&bytes(root+"/"+store.relative_path())==inspection_before);
    const auto diagnostic=inspect(true,"host_reconciliation");
    assert(diagnostic.find(pending.key)!=std::string::npos);
    assert(diagnostic.find(pending.arguments_sha256)!=std::string::npos);
    assert(diagnostic.find("resp_pending")==std::string::npos&&diagnostic.find("call-one")==std::string::npos);
    assert(bytes(root+"/"+store.relative_path())==inspection_before);
    {
        std::ofstream damaged((root+"/"+store.relative_path()).c_str(),std::ios::binary|std::ios::trunc);
        damaged << "{\"tool_effects\":[],\"tool_effects\":[]}";
    }
    const auto damaged_bytes=bytes(root+"/"+store.relative_path());
    inspect(false,"");assert(bytes(root+"/"+store.relative_path())==damaged_bytes);
    assert(preserve_cancelled_coding_progress(cancelled_task,project).find("host inspection")!=std::string::npos);
    assert(!cancelled_task->recovery_available&&bytes(root+"/"+store.relative_path())==damaged_bytes);
    assert(store.save(progress,error));
#endif
    invoke(true,0,"","unconfirmed tool effect requires host reconciliation");
    provider_config_t changed=provider;changed.id="changed";std::string initial,reasoning;size_t calls=0;bool resumed=false;
    const auto before=bytes(root+"/"+store.relative_path());
    assert(!load_matching_coding_progress(store,changed,project,"a new plan",initial,reasoning,calls,resumed,error));
    assert(error.find(pending.key)!=std::string::npos&&bytes(root+"/"+store.relative_path())==before);
#if defined(__unix__) || defined(__APPLE__)
    {
    auto receipt=[&](const std::string &revision,const std::string &key,const std::string &outcome,
        const std::string &output,const std::string &reference,const std::string &staged="[]"){
        acl::json args;auto &a=args.create_node();
        a.add_text("project_path",project.c_str()).add_text("session_id",session_id.c_str())
            .add_text("checkpoint_revision",revision.c_str()).add_text("key",key.c_str())
            .add_text("outcome",outcome.c_str()).add_text("output_json",output.c_str()).add_text("reference",reference.c_str());
        auto &changes=args.create_array();a.add_child("staged_changes",changes);
        libai_schema_json::Document wire,proposals;
        libai::governance_detail::parse_json(std::string(a.to_string().c_str()),wire);
        libai::governance_detail::parse_json(staged,proposals);
        wire["staged_changes"].CopyFrom(proposals,wire.GetAllocator());
        libai_schema_json::StringBuffer buffer;libai_schema_json::Writer<libai_schema_json::StringBuffer> writer(buffer);wire.Accept(writer);
        return coding_service_call(root,"progress.confirm",std::string(buffer.GetString(),buffer.GetSize()));
    };
    agent_progress_t checked;bool found=false;std::string revision;
    assert(store.load_snapshot(checked,found,revision,error)&&found);
    {
        int ready[2],release[2];assert(pipe(ready)==0&&pipe(release)==0);const auto child=fork();assert(child>=0);
        if(!child){
            close(ready[0]);close(release[1]);libai::file_execution_store_t lease(root+"/"+project+"/.libai_agent",libai::execution_file_codec_t::plaintext());
            lease.open("coding_loop_"+session_id);assert(write(ready[1],"r",1)==1);char signal=0;assert(read(release[0],&signal,1)==1);_exit(0);
        }
        close(ready[1]);close(release[0]);char signal=0;assert(read(ready[0],&signal,1)==1);
        acl::json denied(receipt(revision,pending.key,"none","","host-receipt").c_str());
        assert(denied.finish()&&!json_bool(denied["ok"],false)&&bytes(root+"/"+store.relative_path())==before);
        assert(write(release[1],"x",1)==1);int status=0;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
        close(ready[0]);close(release[1]);
    }
    for(const auto &reply:std::vector<std::string>{
        receipt(std::string(64,'b'),pending.key,"none","","receipt-none"),
        receipt(revision,"wrong-key","none","","receipt-none"),
        receipt(revision,pending.key,"none","",""),
        receipt(revision,pending.key,"invalid","","receipt-none"),
        receipt(revision,pending.key,"none","{}","receipt-none"),
        receipt(revision,pending.key,"applied","not json","receipt-applied"),
        receipt(revision,pending.key,"applied",R"({"ok":true,"results":[]})","receipt-applied"),
        receipt(revision,pending.key,"applied",R"({"ok":true,"results":[{"result":5},{"result":6}]})","receipt-applied")}) {
        acl::json denied(reply.c_str());assert(denied.finish()&&!json_bool(denied["ok"],false));
        assert(bytes(root+"/"+store.relative_path())==before);
    }
    {
        const auto result=receipt(revision,pending.key,"none","","external-receipt-none");
        acl::json accepted(result.c_str());assert(accepted.finish()&&json_bool(accepted["ok"],false));
        assert(store.load_snapshot(checked,found,revision,error)&&found);
        assert(checked.tool_effects.back().state=="confirmed_none"&&checked.completed_tool_calls==2);
        assert(checked.tool_effects.back().reference=="external-receipt-none");
        assert(checked.provider_response_id=="resp_pending"&&checked.provider_tool_outputs.size()==2);
        assert(checked.provider_tool_outputs[0].call_id=="call-one"&&checked.provider_tool_outputs[1].call_id=="call-two");
        assert(checked.provider_tool_outputs[0].output.find("no effect")!=std::string::npos);
        invoke(true,2,"resp_pending","cancelled",false);
        const auto committed=bytes(root+"/"+store.relative_path());
        acl::json duplicate(receipt(revision,pending.key,"none","","external-receipt-none").c_str());
        assert(duplicate.finish()&&!json_bool(duplicate["ok"],false)&&bytes(root+"/"+store.relative_path())==committed);
    }
    assert(store.save(progress,error));assert(store.load_snapshot(checked,found,revision,error)&&found);
    {
        const auto output=R"({"ok":true,"results":[{"result":"{\"ok\":true,\"value\":1}"},{"result":"{\"ok\":true,\"value\":2}"}]})";
        acl::json accepted(receipt(revision,pending.key,"applied",output,"external-receipt-applied").c_str());
        assert(accepted.finish()&&json_bool(accepted["ok"],false));
        assert(store.load_snapshot(checked,found,revision,error)&&found);
        assert(checked.tool_effects.back().state=="confirmed_applied"&&checked.completed_tool_calls==2);
        assert(checked.tool_effects.back().output_sha256==agent_workspace_t::content_sha256(output));
        assert(checked.provider_tool_outputs.size()==2&&checked.provider_tool_outputs[1].output==R"({"ok":true,"value":2})");
        assert(checked.transcript.find("host_tool_receipt")!=std::string::npos);
        invoke(true,2,"resp_pending","cancelled",false);
    }
    assert(store.save(progress,error));
    {
        agent_change_proposal_t old;old.operation="write";old.path=project+"/README.md";old.content="# Before";
        old.creates_file=true;old.generation=1;old.review_status="pending";
        old.draft_hash=agent_workspace_t::content_sha256(old.operation+"\n"+old.path+"\n\n"+old.content);
        auto with_draft=progress;with_draft.staged_changes.push_back(old);assert(store.save(with_draft,error));
        assert(store.load_snapshot(checked,found,revision,error)&&found);
        const auto original=bytes(root+"/"+store.relative_path());
        auto proposals=[&](unsigned long long generation,const std::string &review,const std::string &content,const std::string &path){
            acl::json document;auto &item=document.create_node();
            item.add_text("operation","write").add_text("path",path.c_str()).add_text("target_path","").add_text("content",content.c_str()).add_text("reason","host verified")
                .add_bool("creates_file",true).add_bool("creates_directory",false).add_text("original_content","").add_bool("original_content_available",false)
                .add_number("generation",generation).add_text("base_hash","").add_text("draft_hash",agent_workspace_t::content_sha256("write\n"+path+"\n\n"+content).c_str()).add_text("review_status",review.c_str());
            return "["+std::string(item.to_string().c_str())+"]";
        };
        const auto output=R"({"ok":true,"results":[{"result":"{\"ok\":true}"},{"result":"{\"ok\":true}"}]})";
        for(const auto &staged:std::vector<std::string>{"[]",proposals(1,"accepted",old.content,old.path),
            proposals(1,"pending","# Changed",old.path),proposals(2,"accepted","# Changed",old.path),
            proposals(9223372036854775807ULL,"pending","# Changed",old.path),
            proposals(2,"pending","# Changed","other/README.md")}) {
            acl::json rejected(receipt(revision,pending.key,"applied",output,"host-proposal-receipt",staged).c_str());
            assert(rejected.finish()&&!json_bool(rejected["ok"],false)&&bytes(root+"/"+store.relative_path())==original);
        }
        acl::json accepted(receipt(revision,pending.key,"applied",output,"host-proposal-receipt",proposals(2,"pending","# Changed",old.path)).c_str());
        assert(accepted.finish()&&json_bool(accepted["ok"],false));
        assert(store.load_snapshot(checked,found,revision,error)&&found);
        assert(checked.staged_changes.size()==1&&checked.staged_changes[0].generation==2&&checked.staged_changes[0].review_status=="pending");
        assert(bytes(root+"/"+old.path).empty()); // Confirmation records drafts; never applies files.
        assert(store.save(progress,error));
    }
    }
#endif
    progress.tool_effects.clear();assert(store.save(progress,error));
#if defined(__unix__) || defined(__APPLE__)
    invoke(true,0,"resp_original","cancelled",true,true);
#endif
    progress.completed_tool_calls=1;assert(store.save(progress,error));
    invoke(true,1,"resp_original","cancelled");
    invoke(true,0,"","checkpoint identity or cursor changed");
    progress.completed_tool_calls=0;progress.provider_id="changed";assert(store.save(progress,error));
    invoke(true,0,"","checkpoint identity or cursor changed");
    progress.provider_id=provider.id;progress.provider_model="changed";assert(store.save(progress,error));
    invoke(true,0,"","checkpoint identity or cursor changed");
    progress.provider_model=provider.model;progress.original_prompt="changed";assert(store.save(progress,error));
    invoke(true,0,"","checkpoint identity or cursor changed");
    assert(store.remove(error));invoke(true,0,"","checkpoint identity or cursor changed");
#if defined(__unix__) || defined(__APPLE__)
    inspect(true,"new_task");
    agent_progress_t missing;bool found=true;std::string revision="stale";
    assert(store.load_snapshot(missing,found,revision,error)&&!found&&revision.empty());
    assert(!store.save_if_revision(progress,std::string(64,'a'),revision,error)&&revision.empty());
#endif
}
