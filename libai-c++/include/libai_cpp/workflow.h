#pragma once
#include "types.h"
#include <map>
namespace libai { namespace cpp {
struct WorkflowNodeResult {
    std::string patch_json="{}",route,error;
    RunStatus status=RunStatus::completed;
};
struct WorkflowContext {
    ToolContext control;
    // Namespace for a durable child Agent; not a business operation's idempotency key.
    std::string execution_scope;
    bool stopped()const{return control.stopped();}
};
using WorkflowHandler=std::function<WorkflowNodeResult(const std::string&,const WorkflowContext&)>;
struct WorkflowNode {
    std::string name,execution_revision="1";
    std::vector<std::string> writes;
    bool parallel_safe=false,side_effect_free=false;
    WorkflowHandler execute;
};
struct WorkflowOptions {
    // Separate lease owner from any child Agent's store.
    std::shared_ptr<ExecutionStore> execution_store;
    std::string scope;
    std::size_t max_parallel=1,max_node_calls=64,max_state_bytes=2*1024*1024;
    std::chrono::milliseconds timeout{300000};
    std::shared_ptr<Cancellation> cancellation;
};
struct WorkflowResult {
    bool progress_available=true;
    std::string journal_revision,state_json="{}",error;
    RunStatus status=RunStatus::completed;
    TaskContract task_contract;
    std::map<std::string,std::string> nodes;
    std::size_t node_calls=0;
};
// Independent compiled facade. Reject graph mutation/concurrent/reentrant runs;
// callers must not destroy a Workflow while its operations/callbacks are active.
class LIBAI_CPP_API Workflow {
public:
    Workflow();
    ~Workflow();
    Workflow(const Workflow&)=delete;
    Workflow &operator=(const Workflow&)=delete;
    Workflow &add_node(const WorkflowNode &node);
    Workflow &add_edge(const std::string &from,const std::string &to,const std::string &label="");
    WorkflowResult run(const std::string &initial,const WorkflowOptions &options=WorkflowOptions());
    // Trusted host receipt only. Never register this method as an unrestricted model tool.
    void confirm_node(std::shared_ptr<ExecutionStore> store,const std::string &scope,
        const std::string &initial,const std::string &expected_revision,const std::string &node,
        const std::string &outcome,const WorkflowNodeResult &output,const std::string &reference);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}}
