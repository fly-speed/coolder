#include <libai_cpp/workflow.h>
#include "value_bridge.h"
#include "execution_bridge.h"
#include <libai/workflow.h>
namespace libai { namespace cpp {
namespace {
::libai::run_status_t status(RunStatus s){switch(s){
    case RunStatus::completed:return ::libai::run_status_t::completed;
    case RunStatus::cancelled:return ::libai::run_status_t::cancelled;
    case RunStatus::deadline_exceeded:return ::libai::run_status_t::deadline_exceeded;
    case RunStatus::budget_exceeded:return ::libai::run_status_t::budget_exceeded;
    case RunStatus::model_error:return ::libai::run_status_t::model_error;
    case RunStatus::protocol_error:return ::libai::run_status_t::protocol_error;
    case RunStatus::callback_error:return ::libai::run_status_t::callback_error;
    case RunStatus::recovery_required:return ::libai::run_status_t::recovery_required;
}throw std::invalid_argument("workflow status invalid");}
::libai::workflow_node_result_t output(const WorkflowNodeResult &source){
    ::libai::workflow_node_result_t target;target.patch_json=source.patch_json;target.route=source.route;
    target.error=source.error;target.status=status(source.status);return target;
}
}
struct Workflow::Impl {::libai::workflow_t runtime;};
Workflow::Workflow():impl_(new Impl){}
Workflow::~Workflow()=default;
Workflow &Workflow::add_node(const WorkflowNode &source){
    if(!source.execute)throw std::invalid_argument("workflow handler is empty");
    ::libai::workflow_node_t node;node.name=source.name;node.execution_revision=source.execution_revision;
    node.writes=source.writes;node.parallel_safe=source.parallel_safe;node.side_effect_free=source.side_effect_free;
    auto handler=source.execute;
    node.execute=[handler](const std::string &state,const ::libai::workflow_context_t &source){
        WorkflowContext target;target.execution_scope=source.execution_scope;
        target.control.deadline=source.control.deadline;target.control.idempotency_key=source.control.idempotency_key;
        if(source.control.cancellation){target.control.cancellation=std::make_shared<Cancellation>();target.control.cancellation->impl_->native=source.control.cancellation;}
        return output(handler(state,target));
    };
    impl_->runtime.add_node(node);return *this;
}
Workflow &Workflow::add_edge(const std::string &from,const std::string &to,const std::string &label){impl_->runtime.add_edge(from,to,label);return *this;}
WorkflowResult Workflow::run(const std::string &initial,const WorkflowOptions &options){
    ::libai::workflow_options_t native;native.scope=options.scope;native.max_parallel=options.max_parallel;
    native.max_node_calls=options.max_node_calls;native.max_state_bytes=options.max_state_bytes;native.timeout=options.timeout;
    if(options.execution_store)native.execution_store=std::make_shared<detail::ExecutionAdapter>(options.execution_store);
    if(options.cancellation)native.cancellation=options.cancellation->impl_->native;
    const auto result=impl_->runtime.run(initial,native);
    // Reuse the existing explicit status/evidence mapping, without exposing SDK values.
    ::libai::run_result_t summary;summary.status=result.status;summary.task_contract=result.task_contract;
    const auto mapped=detail::result_value(summary);WorkflowResult target;
    target.status=mapped.status;target.task_contract=mapped.task_contract;target.progress_available=result.progress_available;
    target.journal_revision=result.journal_revision;target.state_json=result.state_json;target.error=result.error;
    target.nodes=result.nodes;target.node_calls=result.node_calls;return target;
}
void Workflow::confirm_node(std::shared_ptr<ExecutionStore> store,const std::string &scope,
    const std::string &initial,const std::string &revision,const std::string &node,
    const std::string &outcome,const WorkflowNodeResult &result,const std::string &reference){
    if(!store)throw std::invalid_argument("workflow store is null");
    impl_->runtime.confirm_node(std::make_shared<detail::ExecutionAdapter>(store),scope,initial,revision,node,outcome,output(result),reference);
}
}}
