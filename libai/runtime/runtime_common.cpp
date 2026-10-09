#include "stdafx.h"
#include "../common/json_value.h"
#include "coding_runtime.h"
namespace action
{
namespace agent_detail
{
bool load_review_run_record(const std::string &user_root,
			    const std::string &run_id,
			    const std::string &session_id,
			    webcool::ai::agent_run_record_t &record,
			    std::string &err)
{
	webcool::ai::agent_run_store_t runs(user_root);
	if (runs.get(run_id, record, err))
		return true;
	if (err != "agent run not found" || session_id.empty())
		return false;
	webcool::ai::agent_session_store_t sessions(user_root);
	webcool::ai::agent_session_record_t session;
	if (!sessions.get(session_id, session, err))
		return false;
	webcool::ai::agent_result_store_t results(user_root,
						  session.project_path);
	webcool::ai::agent_result_t saved;
	bool found = false;
	if (!results.load(run_id, saved, found, err))
		return false;
	if (!found || saved.session_id != session.id) {
		err = "agent run not found";
		return false;
	}
	record = webcool::ai::agent_run_record_t();
	record.id = run_id;
	record.agent_id = "coding";
	record.status = "archived";
	record.project_path = saved.project_path;
	record.started_at = saved.saved_at;
	record.finished_at = saved.saved_at;
	err.clear();
	return true;
}

bool reviewable_agent_run_status(const std::string &status)
{
	return status == "running" || status == "completed" ||
	       status == "failed" || status == "cancelled" ||
	       status == "archived";
}

std::string json_text(acl::json_node *node)
{
	return ::webcool::ai::json_value::scalar_text(node);
}

long long json_number(acl::json_node *node, long long fallback)
{
	return ::webcool::ai::json_value::number(node, fallback);
}

bool json_bool(acl::json_node *node, bool fallback)
{
	return ::webcool::ai::json_value::text_boolean(node, fallback);
}

acl::json_node *json_array_node(acl::json_node *node)
{
	return ::webcool::ai::json_value::array_value(node);
}

bool parse_string_array(acl::json_node *node, size_t limit,
			std::vector<std::string> &values)
{
	values.clear();
	acl::json_node *array = json_array_node(node);
	if (array == NULL)
		return node == NULL;
	for (acl::json_node *item = array->first_child(); item != NULL;
	     item = array->next_child()) {
		if (!item->is_string() || values.size() >= limit)
			return false;
		values.push_back(json_text(item));
	}
	return true;
}
std::string serialize_json(acl::json_node &root)
{
	const acl::string &value = root.to_string();
	return std::string(value.c_str(), value.size());
}

}
}
