#include "stdafx.h"
#include "task_contract_store.h"
#include "agent_session_store.h"
#include "../workspace/agent_workspace.h"
#include "../common/file_ops.h"
#include "../common/identifiers.h"
#include "../common/json_value.h"
#include "../common/webcool_mutex.h"
namespace webcool
{
namespace ai
{
namespace
{
webcool::mutex contract_mutex;
struct request_t {
	std::string run, text;
};
bool decode(const std::string &value, std::vector<request_t> &requests)
{
	acl::json json(value.c_str());
	if (!json.finish() || json_value::number(json["version"]) != 1)
		return false;
	auto *array = json_value::array_value(json["requests"]);
	if (!array)
		return false;
	for (auto *item = array->first_child(); item;
	     item = array->next_child()) {
		request_t request;
		request.run = json_value::nullable_text(
			json_value::object_child(item, "run_id"));
		request.text = json_value::nullable_text(
			json_value::object_child(item, "text"));
		if (request.text.empty() || requests.size() >= 256)
			return false;
		requests.push_back(request);
	}
	return true;
}
}
bool begin_task_contract(const std::string &user_root,
			 const std::string &project,
			 const std::string &session_id,
			 const std::string &run_id, const std::string &prompt,
			 bool resume, std::string &contract, std::string &error)
{
	std::lock_guard<webcool::mutex> lock(contract_mutex);
	if (!identifiers::valid_id(run_id) ||
	    (!session_id.empty() && !identifiers::valid_id(session_id))) {
		error = "invalid task contract identity";
		return false;
	}
	std::string root;
	if (!agent_workspace_t::resolve_project_state_root(user_root, project,
							   root, error))
		return false;
	const std::string directory =
		file_ops::join_path(root, ".webcool_agent");
	if (!file_ops::make_private_directory(directory)) {
		error = "cannot create safe task contract directory";
		return false;
	}
	agent_workspace_t storage(directory);
	const std::string name = "task-contract-" +
				 (session_id.empty() ? run_id : session_id) +
				 ".json";
	std::vector<request_t> requests;
	bool bootstrapped = false;
	if (file_ops::path_entry_exists(file_ops::join_path(directory, name))) {
		std::string value;
		bool truncated = false;
		if (!storage.read(name, value, truncated, error))
			return false;
		if (truncated || !decode(value, requests)) {
			error = "task contract is invalid or too large; refusing to discard requirements";
			return false;
		}
	} else if (!session_id.empty()) {
		agent_session_record_t session;
		if (!agent_session_store_t(user_root).get(session_id, session,
							  error))
			return false;
		if (session.project_path != project) {
			error = "task contract project mismatch";
			return false;
		}
		for (const auto &message : session.messages)
			if (message.role == "user")
				requests.push_back(
					{ message.run_id, message.text });
		bootstrapped = !requests.empty();
	}
	bool duplicate = false;
	for (const auto &request : requests)
		if (request.run == run_id)
			duplicate = true;
	if (resume && !requests.empty() && requests.back().text == prompt)
		duplicate = true;
	if (!duplicate)
		requests.push_back({ run_id, prompt });
	size_t bytes = 0;
	for (const auto &request : requests)
		bytes += request.text.size();
	if (bytes > 64 * 1024 || requests.size() > 256) {
		error = "task contract exceeds context capacity; start a new scoped session (requirements were not truncated)";
		return false;
	}
	acl::json json;
	auto &node = json.create_node();
	node.add_number("version", 1);
	node.add_text("project_path", project.c_str());
	node.add_text("session_id", session_id.c_str());
	node.add_text("authority", "user_requests");
	node.add_text(
		"history_limit",
		"Older sessions import only retained user messages; missing history is unknown.");
	node.add_bool("bootstrapped_from_history", bootstrapped);
	auto &array = json.create_array();
	node.add_child("requests", array);
	size_t number = 0;
	for (const auto &request : requests) {
		auto &item = array.add_child(false, true);
		item.add_text("id", ("U" + std::to_string(++number)).c_str());
		item.add_text("source", "user");
		item.add_text("run_id", request.run.c_str());
		item.add_text("text", request.text.c_str());
	}
	contract = node.to_string().c_str();
	if (contract.size() > 128 * 1024) {
		error = "encoded task contract exceeds context capacity; requirements were not truncated";
		return false;
	}
	return storage.save_generated_text(name, contract, error);
}
}
}
