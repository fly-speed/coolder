#pragma once
#include <libai/workspace/workspace_change_set.h>
// Invoked by the full-backend recovery test; no real user workspace is touched.
static void workspace_transaction_leases(const std::string &root)
{
#if defined(__unix__) || defined(__APPLE__)
    const auto scope=root+"/workspace-leases-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    assert(file_ops::make_private_directory(scope));std::ofstream(scope+"/one.txt")<<"before";
    workspace_change_set_store_t store(scope);workspace_change_input_t change;
    change.operation="write";change.path="one.txt";change.content="after";
    workspace_change_set_preview_t preview;std::string error;std::vector<workspace_change_result_t> results;
    assert(store.create({change},preview,error));const auto id=preview.id;
    {
        libai::file_execution_store_t lease(scope+"/.libai_agent/change_sets",libai::execution_file_codec_t::plaintext());
        lease.open("workspace_transaction");
        assert(!store.apply(id,results,error)&&bytes(scope+"/one.txt")=="before");
        assert(!store.recover(error)&&bytes(scope+"/one.txt")=="before");
        assert(!store.create({change},preview,error)&&bytes(scope+"/one.txt")=="before");
        assert(!store.create_and_apply({change},preview,results,error)&&bytes(scope+"/one.txt")=="before");
    }
    int barrier[2];assert(pipe(barrier)==0);std::vector<pid_t> children;
    for(unsigned ordinal=0;ordinal<2;++ordinal) {
        const auto child=fork();assert(child>=0);
        if(child==0) {
            close(barrier[1]);char signal;assert(read(barrier[0],&signal,1)==1);close(barrier[0]);
            for(unsigned attempt=0;attempt<200;++attempt) {
                std::string reason;std::vector<workspace_change_result_t> applied;
                if(store.apply(id,applied,reason))_exit(0);
                if(reason.find("workspace transaction lease unavailable")==0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));continue;
                }
                if(reason.find("not found or already consumed")!=std::string::npos)_exit(1);
                _exit(2);
            }
            _exit(3);
        }
        children.push_back(child);
    }
    close(barrier[0]);assert(write(barrier[1],"xx",2)==2);close(barrier[1]);unsigned successes=0;
    for(const auto child:children) {
        int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status));
        assert(WEXITSTATUS(status)==0||WEXITSTATUS(status)==1);successes+=WEXITSTATUS(status)==0;
    }
    assert(successes==1&&bytes(scope+"/one.txt")=="after");
    assert(!store.apply(id,results,error)&&bytes(scope+"/one.txt")=="after");
    assert(store.recover(error)&&bytes(scope+"/one.txt")=="after");
#else
    (void)root;
#endif
}
