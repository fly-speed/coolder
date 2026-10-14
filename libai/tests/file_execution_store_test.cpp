#include <libai/agent.h>
#include <libai/sdk/file_execution_store.h>
#include <cassert>
#include <dirent.h>
#include <fstream>
#include <sys/wait.h>
using namespace libai;
template<class F> void rejects(F f) { bool failed=false;try{f();}catch(const std::exception&){failed=true;}assert(failed); }
int main() {
    char path[]="/tmp/libai-store-XXXXXX"; assert(mkdtemp(path)); std::string root=path;
    auto codec=execution_file_codec_t::plaintext();
    file_execution_store_t a(root,codec), b(root,codec);
    assert(a.open("one").revision.empty());
    rejects([&]{b.open("one");});
    a.close("other");rejects([&]{b.open("one");});
    assert(a.commit("one","","{}") == "1");
    rejects([&]{a.commit("one","","{\"x\":1}");});
    a.close("one");
    {
        std::ifstream file(root+"/one.state");std::string raw((std::istreambuf_iterator<char>(file)),{});
        governance_detail::json::Document envelope;governance_detail::parse_json(raw,envelope);
        assert(envelope["version"].GetUint()==1 && std::string(envelope["scope"].GetString())=="one");
        assert(std::string(envelope["encoding"].GetString())=="hex" && std::string(envelope["payload"].GetString())=="7b7d");
    }
    {
        std::ofstream corrupt(root+"/broken.state");corrupt<<"01\n{}";
    }
    assert(chmod((root+"/broken.state").c_str(),0600)==0);rejects([&]{a.open("broken");});
    unlink((root+"/broken.state").c_str());assert(a.open("broken").revision.empty());a.close("broken");
    assert(symlink(root.c_str(),(root+"/alias").c_str())==0);
    rejects([&]{file_execution_store_t alias(root+"/alias/",codec);});unlink((root+"/alias").c_str());
    auto snapshot=b.open("one");assert(snapshot.revision=="1" && snapshot.state_json=="{}");
    b.close("one");rejects([&]{a.open("../escape");});
    assert(symlink("one.state",(root+"/link.state").c_str())==0);
    rejects([&]{a.open("link");});
    // An open failure must release the acquired lock, not poison later attempts.
    unlink((root+"/link.state").c_str());assert(a.open("link").revision.empty());a.close("link");
    assert(mkfifo((root+"/fifo.state").c_str(),0600)==0);rejects([&]{a.open("fifo");});
    unlink((root+"/fifo.state").c_str());
    assert(chmod(root.c_str(),0755)==0);rejects([&]{file_execution_store_t unsafe(root,codec);});assert(chmod(root.c_str(),0700)==0);
    bool fail_encode=true;
    auto custom=codec;custom.encode=[&](const std::string &scope,const std::string &revision,const std::string &state){
        assert(scope=="codec" && revision=="1");if(fail_encode)throw std::runtime_error("codec failed");return state;
    };
    file_execution_store_t coded(root,custom);coded.open("codec");rejects([&]{coded.commit("codec","","{}");});
    coded.close("codec");assert(coded.open("codec").revision.empty());fail_encode=false;assert(coded.commit("codec","","{}") == "1");coded.close("codec");
    for (const char *invalid : {
        "{\"version\":1,\"scope\":\"other\",\"revision\":\"1\",\"encoding\":\"hex\",\"payload\":\"7b7d\"}",
        "{\"version\":1,\"scope\":\"invalid\",\"revision\":\"1\",\"encoding\":\"hex\",\"payload\":\"zz\"}",
        "{\"version\":1,\"version\":1}"}) {
        {std::ofstream file(root+"/invalid.state");file<<invalid;}
        assert(chmod((root+"/invalid.state").c_str(),0600)==0);
        rejects([&]{a.open("invalid");});
    }
    unlink((root+"/invalid.state").c_str());
    {
        const std::string large="{\"v\":\""+std::string(2*1024*1024-8,'x')+"\"}";
        assert(a.open("large").revision.empty());assert(a.commit("large","",large)=="1");a.close("large");
        assert(a.open("large").state_json==large);a.close("large");
        std::ifstream file(root+"/large.state");std::string raw((std::istreambuf_iterator<char>(file)),{});
        governance_detail::json::Document value;
        rejects([&]{governance_detail::parse_json(raw,value);}); // Model-facing byte bound unchanged.
    }
    {
        auto binary=codec;
        binary.encode=[](const std::string &scope,const std::string &revision,const std::string &data){
            assert(scope=="binary" && revision=="1");return std::string("\0\xff",2)+data;
        };
        binary.decode=[](const std::string &scope,const std::string &revision,const std::string &data){
            assert(scope=="binary" && revision=="1" && data.substr(0,2)==std::string("\0\xff",2));return data.substr(2);
        };
        file_execution_store_t store(root,binary);store.open("binary");store.commit("binary","","{}");store.close("binary");
        assert(store.open("binary").state_json=="{}");store.close("binary");
    }
    // Real process exits after a durable external effect, before the result checkpoint.
    pid_t child=fork();assert(child>=0);
    if(!child){
        auto store=std::make_shared<file_execution_store_t>(root,codec);
        agent_t agent(std::make_shared<function_model_t>([](const model_request_t &,const stream_sink_t &){
            model_response_t r;tool_call_t c;c.id="a";c.name="write";r.calls={c};return r;
        }));tool_spec_t spec;spec.name="write";
        agent.add_tool(spec,[&](const tool_call_t &,const tool_context_t &context)->tool_result_t{
            assert(context.idempotency_key=="crash.0");int fd=::open((root+"/effect").c_str(),O_WRONLY|O_CREAT|O_APPEND,0600);assert(fd>=0);
            assert(write(fd,"x",1)==1 && fsync(fd)==0);::close(fd);_exit(77);
        });run_options_t options;options.execution_store=store;options.execution_scope="crash";agent_session_t session;agent.run(session,"input",options);_exit(78);
    }
    int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==77);
    auto store=std::make_shared<file_execution_store_t>(root,codec);int calls=0;
    agent_t agent(std::make_shared<function_model_t>([&](const model_request_t &,const stream_sink_t &){++calls;model_response_t r;r.text="done";return r;}));
    tool_spec_t spec;spec.name="write";agent.add_tool(spec,[](const tool_call_t &,const tool_context_t &)->tool_result_t{assert(false);return tool_result_t();});
    run_options_t options;options.execution_store=store;options.execution_scope="crash";agent_session_t session;
    assert(agent.run(session,"input",options).status==run_status_t::recovery_required && calls==0);
    snapshot=store->open("crash");store->close("crash");
    {
        std::ofstream legacy(root+"/crash.state",std::ios::binary|std::ios::trunc);
        legacy<<snapshot.revision<<"\n"<<snapshot.state_json;
    }
    assert(agent.run(session,"input",options).status==run_status_t::recovery_required && calls==0);
    {
        std::ifstream legacy(root+"/crash.state");char first;legacy.get(first);assert(first!='{');
    }
    confirm_execution_effect(store,"crash","input","",snapshot.revision,"crash.0",tool_result_t::success("written"),"applied","effect/one-byte");
    {
        std::ifstream file(root+"/crash.state");std::string raw((std::istreambuf_iterator<char>(file)),{});
        governance_detail::json::Document envelope;governance_detail::parse_json(raw,envelope);
        assert(envelope["version"].GetUint()==1);
    }
    assert(agent.run(session,"input",options).status==run_status_t::completed && calls==1);
    assert(agent.run(session,"input",options).status==run_status_t::completed && calls==1);
    struct stat st;assert(stat((root+"/effect").c_str(),&st)==0 && st.st_size==1);
    DIR *directory=opendir(root.c_str());assert(directory);while(auto *entry=readdir(directory)){
        std::string name=entry->d_name;if(name!="." && name!="..")assert(unlink((root+"/"+name).c_str())==0);
    }closedir(directory);assert(rmdir(root.c_str())==0);
}
