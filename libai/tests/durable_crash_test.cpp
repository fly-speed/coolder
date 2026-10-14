#include <libai/agent.h>
#include <cassert>
#include <fstream>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace libai;
// Test adapter: private directory, process lease, atomic rename and fsync.
class FileStore:public execution_store_t {
    std::string root_;int lease_=-1;
    execution_snapshot_t read() {
        execution_snapshot_t s;std::ifstream in(root_+"/state",std::ios::binary);
        if(!in)return s;
        std::getline(in,s.revision);s.state_json.assign(std::istreambuf_iterator<char>(in),{});return s;
    }
public:
    explicit FileStore(std::string root):root_(std::move(root)){}
    execution_snapshot_t open(const std::string &) override {
        if(lease_!=-1)throw std::runtime_error("already leased");
        lease_=::open((root_+"/lock").c_str(),O_RDWR|O_CREAT|O_NOFOLLOW,0600);
        if(lease_<0 || flock(lease_,LOCK_EX|LOCK_NB)!=0){close("");throw std::runtime_error("lease unavailable");}
        return read();
    }
    std::string commit(const std::string &,const std::string &expected,const std::string &state) override {
        if(lease_<0)throw std::runtime_error("unleased");
        auto previous=read();if(previous.revision!=expected)throw std::runtime_error("revision conflict");
        auto revision=std::to_string(expected.empty()?1:std::stoull(expected)+1);
        auto bytes=revision+"\n"+state;
        int fd=::open((root_+"/temporary").c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);assert(fd>=0);
        std::size_t offset=0;while(offset<bytes.size()){auto n=write(fd,bytes.data()+offset,bytes.size()-offset);assert(n>0);offset+=n;}
        assert(fsync(fd)==0);assert(::close(fd)==0);
        assert(rename((root_+"/temporary").c_str(),(root_+"/state").c_str())==0);
        fd=::open(root_.c_str(),O_RDONLY);assert(fd>=0);assert(fsync(fd)==0);::close(fd);return revision;
    }
    void close(const std::string &) noexcept override {if(lease_>=0){flock(lease_,LOCK_UN);::close(lease_);lease_=-1;}}
};
int main() {
    char name[]="/tmp/libai-crash-XXXXXX";char *dir=mkdtemp(name);assert(dir);std::string root=dir;
    pid_t child=fork();assert(child>=0);
    if(child==0) {
        auto store=std::make_shared<FileStore>(root);
        auto model=std::make_shared<function_model_t>([](const model_request_t &,const stream_sink_t &){model_response_t r;tool_call_t c;c.id="charge";c.name="charge";r.calls={c};return r;});
        agent_t agent(model);tool_spec_t tool;tool.name="charge";
        agent.add_tool(tool,[&](const tool_call_t &,const tool_context_t &context)->tool_result_t {
            assert(context.idempotency_key=="crash_task.0");
            int fd=::open((root+"/effect").c_str(),O_WRONLY|O_CREAT|O_APPEND,0600);assert(fd>=0);
            assert(write(fd,"x",1)==1 && fsync(fd)==0);::close(fd);
            _exit(77); // Effect committed externally; no tool receipt reached the SDK.
        });
        run_options_t options;options.execution_store=store;options.execution_scope="crash_task";
        agent_session_t session;agent.run(session,"pay",options);_exit(99);
    }
    int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==77);
    auto store=std::make_shared<FileStore>(root);int models=0,effects=0;
    agent_t resumed(std::make_shared<function_model_t>([&](const model_request_t &request,const stream_sink_t &){++models;assert(request.messages.back().text=="receipt");model_response_t r;r.text="done";return r;}));
    tool_spec_t tool;tool.name="charge";
    resumed.add_tool(tool,[&](const tool_call_t &,const tool_context_t &){++effects;return tool_result_t::success("duplicate");});
    run_options_t options;options.execution_store=store;options.execution_scope="crash_task";agent_session_t session;
    auto blocked=resumed.run(session,"pay",options);
    assert(blocked.status==run_status_t::recovery_required && models==0 && effects==0);
    auto snapshot=store->open("crash_task");
    auto competing=std::make_shared<FileStore>(root);bool busy=false;
    try{competing->open("crash_task");}catch(const std::exception&){busy=true;}assert(busy);
    store->close("crash_task");
    struct stat st;assert(stat((root+"/effect").c_str(),&st)==0 && st.st_size==1);
    confirm_execution_effect(store,"crash_task","pay","",snapshot.revision,"crash_task.0",tool_result_t::success("receipt"),"applied","host:fsynced-effect");
    assert(resumed.run(session,"pay",options).status==run_status_t::completed && models==1 && effects==0);
    assert(resumed.run(session,"pay",options).status==run_status_t::completed && models==1 && effects==0);
    assert(stat((root+"/effect").c_str(),&st)==0 && st.st_size==1);
    for(const char *file:{"state","lock","effect","temporary"})unlink((root+"/"+file).c_str());assert(rmdir(root.c_str())==0);
}
