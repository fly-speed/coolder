#include "stdafx.h"
#include "coding_runtime.h"
#include "../coding/coding_service.h"
namespace action { namespace agent_detail {
namespace {
std::string relative(const std::string &project, const std::string &path)
{
    if (path == project) return "";
    if (path.compare(0, project.size()+1, project+"/") != 0)
        throw std::invalid_argument("task proposal is outside its project");
    return path.substr(project.size()+1);
}
}
void refresh_result_task_contract(const std::string &user_root,
    const std::string &status, webcool::ai::agent_result_t &result)
{
    try {
        std::string project, error;
        if (!webcool::ai::agent_workspace_t::resolve_project_root(
            user_root, result.project_path, project, error))
            throw std::runtime_error(error);
        acl::json input;
        auto &run = input.create_node();
        run.add_text("id", result.run_id.c_str()).add_text("status", status.c_str());
        auto &changes=input.create_array();run.add_child("changes",changes);
        for (const auto &change : result.changes) {
            if (change.review_status == "superseded") continue;
            const auto path=relative(result.project_path,change.path);
            if (path.empty() && change.operation=="mkdir") continue;
            auto &c=changes.add_child(false,true);
            c.add_text("relative_path",path.c_str()).add_text("operation",change.operation.c_str())
                .add_text("content",change.content.c_str()).add_text("review_status",change.review_status.c_str())
                .add_text("accepted_by",change.accepted_by.c_str()).add_text("review_reference",change.review_reference.c_str());
            if (!change.target_path.empty())
                c.add_text("target_path",relative(result.project_path,change.target_path).c_str());
        }
        acl::json acceptance(result.task_acceptance_json.c_str());
        const auto report_text=acceptance.finish()?json_text(acceptance["report"]):"";
        acl::json report(report_text.c_str());
        if (report.finish() && report.get_root().is_object()) {
            // Embed trusted execution report for both independent command classifications.
            run.add_child("validation",report.get_root());
            acl::json test_report(report_text.c_str());
            if (test_report.finish()) run.add_child("test_validation",test_report.get_root());
            // Serialize while both parsed documents still own their nodes.
            result.acceptance_contract_json=webcool::ai::coding_service_call(
                project,"tasks.describe_run",run.to_string().c_str());
        } else {
            result.acceptance_contract_json=webcool::ai::coding_service_call(
                project,"tasks.describe_run",run.to_string().c_str());
        }
    } catch (const std::exception &e) {
        acl::json failure;auto &out=failure.create_node();
        out.add_number("version",1).add_bool("accepted",false).add_text("error",e.what());
        result.acceptance_contract_json=out.to_string().c_str();
    }
}
void add_current_task_contract_json(acl::json_node &root,const std::string &user_root,
    const webcool::ai::agent_run_record_t &record)
{
    webcool::ai::agent_result_store_t store(user_root,record.project_path);
    webcool::ai::agent_result_t result;bool found=false;std::string error;
    if (!store.load(record.id,result,found,error) || !found) return;
    const auto previous=result.acceptance_contract_json;
    refresh_result_task_contract(user_root,record.status,result);
    if (previous!=result.acceptance_contract_json && !store.save(result,error)) {
        acl::json failure;auto &out=failure.create_node();
        out.add_number("version",1).add_bool("accepted",false).add_text("error",error.c_str());
        result.acceptance_contract_json=out.to_string().c_str();
    }
    root.add_text("acceptance_contract_json",result.acceptance_contract_json.c_str());
}
}}
