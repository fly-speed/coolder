#include <libai_cpp/agent.h>
#include <cassert>
#include <cstdlib>
#include <stdexcept>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace libai::cpp;
int main()
{
    assert(file_execution_store_available());
    char directory[]="/tmp/libai-cpp-store-XXXXXX";assert(mkdtemp(directory));std::string root=directory;
    auto codec=ExecutionFileCodec::plaintext();
    auto store=create_file_execution_store(root,codec);
    assert(store->open("check").revision.empty());
    auto rival=create_file_execution_store(root,codec);bool denied=false;
    try{rival->open("check");}catch(const std::exception&){denied=true;}assert(denied);
    assert(store->commit("check","","{}") == "1");store->close("check");
    auto snapshot=rival->open("check");assert(snapshot.revision=="1" && snapshot.state_json=="{}");rival->close("check");
    // The facade preserves host codec context and failure before side effects.
    auto failing=codec;failing.encode=[](const std::string &scope,const std::string &revision,const std::string &)->std::string{
        assert(scope=="blocked" && !revision.empty());throw std::runtime_error("codec unavailable");
    };
    RunOptions blocked;blocked.execution_store=create_file_execution_store(root,failing);blocked.execution_scope="blocked";
    int models=0;
    Agent first([&](const ModelRequest&,const StreamSink&){++models;return ModelResponse();});
    assert(first.run("input",blocked).status==RunStatus::recovery_required && models==0);
    // No leases are live across fork. Child commits an external effect then exits.
    pid_t child=fork();assert(child>=0);
    if(!child){
        Agent agent([](const ModelRequest&,const StreamSink&){ModelResponse r;ToolCall c;c.id="a";c.name="write";r.calls={c};return r;});
        ToolSpec spec;spec.name="write";spec.execution_revision="v1";
        agent.add_tool(spec,[&](const ToolCall&,const ToolContext&context)->ToolResult{
            assert(context.idempotency_key=="crash.0");
            int fd=open((root+"/effect").c_str(),O_WRONLY|O_CREAT|O_APPEND,0600);assert(fd>=0);
            assert(write(fd,"x",1)==1 && fsync(fd)==0);close(fd);_exit(77);
        });
        RunOptions options;options.execution_store=create_file_execution_store(root,codec);options.execution_scope="crash";
        agent.run("input",options);_exit(78);
    }
    int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==77);
    store=create_file_execution_store(root,codec);models=0;
    auto run=[&](){
        Agent agent([&](const ModelRequest&,const StreamSink&){++models;ModelResponse r;r.text="done";return r;});
        ToolSpec spec;spec.name="write";spec.execution_revision="v1";
        agent.add_tool(spec,[](const ToolCall&,const ToolContext&)->ToolResult{assert(false);return ToolResult();});
        RunOptions options;options.execution_store=store;options.execution_scope="crash";return agent.run("input",options);
    };
    assert(run().status==RunStatus::recovery_required && models==0);
    snapshot=store->open("crash");store->close("crash");
    confirm_execution_effect(store,"crash","input","",snapshot.revision,"crash.0",ToolResult::success("written"),"applied","effect/one-byte");
    assert(run().status==RunStatus::completed && models==1);
    assert(run().text=="done" && models==1);
    struct stat st;assert(stat((root+"/effect").c_str(),&st)==0 && st.st_size==1);
    DIR *dir=opendir(root.c_str());assert(dir);
    while(auto *entry=readdir(dir)){std::string name=entry->d_name;if(name!="." && name!="..")assert(unlink((root+"/"+name).c_str())==0);}
    closedir(dir);assert(rmdir(root.c_str())==0);
}
