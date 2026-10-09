#include "stdafx.h"
#include "../common/file_ops.h"
#include "../common/identifiers.h"
#include "../common/record_codec.h"
#include "sandbox_execution.h"
#include "../workspace/agent_workspace.h"
#include "../agent/ai_admin_policy.h"
#include "../common/ai_error_log.h"
#include "../project/project_toolchain.h"
#include "../common/webcool_mutex.h"

#ifdef _WIN32
#include "../common/platform_compat.h"
#include <direct.h>
#else
#include <unistd.h>
#endif

#include <openssl/rand.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <sys/stat.h>
#include <vector>

namespace webcool
{
namespace ai
{
namespace
{

// Each user has one pending plan. pending.v1 is atomically renamed before use;
// sandbox_runs.v1 stores only bounded start/finish metadata. A process mutex
// protects read-modify-replace operations inside one WebCool service process.
webcool::mutex g_execution_store_mutex;
const char *kPlanHeader = "WEBCOOL_SANDBOX_PLAN_V1";
const char *kAuditHeader = "WEBCOOL_SANDBOX_AUDIT_V1";
const long long kPlanLifetimeSeconds = 10 * 60;
const size_t kMaxArguments = 64;
const size_t kMaxAuditRecords = 100;
const size_t kMaxAuditBytes = 1024 * 1024;

using ::webcool::ai::file_ops::join_path;

using ::webcool::ai::record_codec::hex_value;

using ::webcool::ai::record_codec::hex_encode;

using ::webcool::ai::record_codec::hex_decode;

using ::webcool::ai::identifiers::valid_id;

using ::webcool::ai::identifiers::new_id;

using ::webcool::ai::file_ops::safe_directory;

bool ensure_directory(const std::string &path)
{
	if (safe_directory(path))
		return true;
#ifdef _WIN32
	std::wstring wide;
	if (!webcool_utf8_path_to_wide(path.c_str(), wide) ||
	    _wmkdir(wide.c_str()) != 0)
		return false;
#else
	if (mkdir(path.c_str(), 0700) != 0)
		return false;
#endif
	return safe_directory(path);
}

using ::webcool::ai::file_ops::replace_file;

bool move_file(const std::string &source, const std::string &target)
{
#ifdef _WIN32
	std::wstring source_wide;
	std::wstring target_wide;
	return webcool_utf8_path_to_wide(source.c_str(), source_wide) &&
	       webcool_utf8_path_to_wide(target.c_str(), target_wide) &&
	       MoveFileExW(source_wide.c_str(), target_wide.c_str(),
			   MOVEFILE_WRITE_THROUGH) != 0;
#else
	return rename(source.c_str(), target.c_str()) == 0;
#endif
}

bool ensure_store_directory(const std::string &user_root,
			    std::string &directory, std::string &err)
{
	const std::string agent = join_path(user_root, ".webcool_agent");
	directory = join_path(agent, "sandbox_plans");
	if (!ensure_directory(agent) || !ensure_directory(directory)) {
		err = "cannot create a safe sandbox plan directory";
		return false;
	}
#ifndef _WIN32
	if (chmod(agent.c_str(), 0700) != 0 ||
	    chmod(directory.c_str(), 0700) != 0) {
		err = "cannot protect sandbox plan directory";
		return false;
	}
#endif
	return true;
}

std::string pending_path(const std::string &directory)
{
	return join_path(directory, "pending.v1");
}

sandbox_limits_t fixed_limits()
{
	sandbox_limits_t limits = ai_runtime_policy_get().sandbox_limits;
	limits.open_files = 64;
	limits.file_size_bytes = 64ULL * 1024ULL * 1024ULL;
	return limits;
}

bool same_command(const sandbox_command_t &left, const sandbox_command_t &right)
{
	return left.id == right.id && left.executable == right.executable &&
	       left.fixed_arguments == right.fixed_arguments &&
	       !left.allow_dynamic_arguments &&
	       !right.allow_dynamic_arguments
	       // Interactive one-shot execution plans never authorize a listening socket.
	       // Loopback service validation uses a separate server-owned workflow.
	       && !left.allow_loopback_network && !right.allow_loopback_network;
}

bool save_plan(const std::string &directory,
	       const sandbox_execution_plan_t &plan, std::string &err)
{
	const std::string path = pending_path(directory);
	const std::string temporary = path + ".tmp";
	std::ofstream out(temporary.c_str(),
			  std::ios::out | std::ios::binary | std::ios::trunc);
	if (!out.good()) {
		err = "cannot write sandbox execution plan";
		return false;
	}
	out << kPlanHeader << '\n'
	    << plan.created_at << '\n'
	    << plan.id << '\n'
	    << hex_encode(plan.project_path) << '\n'
	    << hex_encode(plan.command.id) << '\n'
	    << hex_encode(plan.command.executable) << '\n'
	    << plan.command.fixed_arguments.size() << '\n';
	for (size_t i = 0; i < plan.command.fixed_arguments.size(); ++i) {
		out << hex_encode(plan.command.fixed_arguments[i]) << '\n';
	}
	out.close();
	if (!out.good()) {
		remove(temporary.c_str());
		err = "cannot flush sandbox execution plan";
		return false;
	}
#ifndef _WIN32
	if (chmod(temporary.c_str(), 0600) != 0) {
		remove(temporary.c_str());
		err = "cannot protect sandbox execution plan";
		return false;
	}
#endif
	if (!replace_file(temporary, path)) {
		remove(temporary.c_str());
		err = std::string("cannot install sandbox execution plan: ") +
		      strerror(errno);
		return false;
	}
	return true;
}

bool load_plan_file(const std::string &path, const std::string &requested_id,
		    sandbox_execution_plan_t &plan, std::string &err)
{
	if (!valid_id(requested_id)) {
		err = "invalid sandbox execution plan id";
		return false;
	}
	std::ifstream in(path.c_str(), std::ios::in | std::ios::binary);
	if (!in.good()) {
		err = "sandbox execution plan not found";
		return false;
	}
	std::string header;
	std::string created;
	std::string argument_count_text;
	std::string encoded;
	if (!std::getline(in, header) || header != kPlanHeader ||
	    !std::getline(in, created) || !std::getline(in, plan.id) ||
	    plan.id != requested_id || !std::getline(in, encoded) ||
	    !hex_decode(encoded, plan.project_path) ||
	    !std::getline(in, encoded) ||
	    !hex_decode(encoded, plan.command.id) ||
	    !std::getline(in, encoded) ||
	    !hex_decode(encoded, plan.command.executable) ||
	    !std::getline(in, argument_count_text)) {
		err = "invalid sandbox execution plan";
		return false;
	}
	char *end = NULL;
	plan.created_at = strtoll(created.c_str(), &end, 10);
	if (end == created.c_str() || *end != '\0') {
		err = "invalid sandbox execution plan timestamp";
		return false;
	}
	end = NULL;
	const unsigned long argument_count =
		strtoul(argument_count_text.c_str(), &end, 10);
	if (end == argument_count_text.c_str() || *end != '\0' ||
	    argument_count > kMaxArguments) {
		err = "invalid sandbox execution plan arguments";
		return false;
	}
	plan.command.fixed_arguments.clear();
	for (unsigned long i = 0; i < argument_count; ++i) {
		if (!std::getline(in, encoded)) {
			err = "invalid sandbox execution plan arguments";
			return false;
		}
		std::string argument;
		if (!hex_decode(encoded, argument) ||
		    argument.size() > 32 * 1024) {
			err = "invalid sandbox execution plan argument";
			return false;
		}
		plan.command.fixed_arguments.push_back(argument);
	}
	plan.command.allow_dynamic_arguments = false;
	plan.limits = fixed_limits();
	return true;
}

bool append_audit(const std::string &user_root,
		  const sandbox_execution_plan_t &plan, const char *event,
		  const sandbox_result_t *result, std::string &err)
{
	std::string directory;
	if (!ensure_store_directory(user_root, directory, err))
		return false;
	const std::string path = join_path(
		join_path(user_root, ".webcool_agent"), "sandbox_runs.v1");
	std::vector<std::string> records;
	std::ifstream in(path.c_str(), std::ios::in | std::ios::binary);
	if (in.good()) {
		std::string line;
		if (std::getline(in, line) && line == kAuditHeader) {
			while (records.size() < kMaxAuditRecords &&
			       std::getline(in, line)) {
				if (!line.empty())
					records.push_back(line);
			}
		}
	}
	if (records.size() >= kMaxAuditRecords)
		records.erase(records.begin());
	std::string result_error = result ? result->error : "";
	if (result_error.size() > 1024)
		result_error.resize(1024);
	std::ostringstream record;
	record << static_cast<long long>(time(NULL)) << '\t' << event << '\t'
	       << plan.id << '\t' << hex_encode(plan.project_path) << '\t'
	       << hex_encode(plan.command.id) << '\t'
	       << (result ? result->exit_code : -1) << '\t'
	       << (result ? result->signal : 0) << '\t'
	       << (result ? result->elapsed_ms : 0) << '\t'
	       << (result && result->timed_out ? 1 : 0) << '\t'
	       << (result && result->output_truncated ? 1 : 0) << '\t'
	       << hex_encode(result_error) << '\t'
	       << (result && result->cancelled ? 1 : 0);
	records.push_back(record.str());
	const std::string temporary = path + ".tmp";
	std::ofstream out(temporary.c_str(),
			  std::ios::out | std::ios::binary | std::ios::trunc);
	if (!out.good()) {
		err = "cannot write sandbox execution audit";
		return false;
	}
	out << kAuditHeader << '\n';
	size_t written = strlen(kAuditHeader) + 1;
	for (size_t i = 0; i < records.size(); ++i) {
		if (written + records[i].size() + 1 > kMaxAuditBytes)
			continue;
		out << records[i] << '\n';
		written += records[i].size() + 1;
	}
	out.close();
	if (!out.good()) {
		remove(temporary.c_str());
		err = "cannot flush sandbox execution audit";
		return false;
	}
#ifndef _WIN32
	if (chmod(temporary.c_str(), 0600) != 0) {
		remove(temporary.c_str());
		err = "cannot protect sandbox execution audit";
		return false;
	}
#endif
	if (!replace_file(temporary, path)) {
		remove(temporary.c_str());
		err = "cannot install sandbox execution audit";
		return false;
	}
	return true;
}

bool parse_integer(const std::string &text, long long minimum,
		   long long maximum, long long &value)
{
	if (text.empty())
		return false;
	char *end = NULL;
	errno = 0;
	const long long parsed = strtoll(text.c_str(), &end, 10);
	if (errno != 0 || end == text.c_str() || *end != '\0' ||
	    parsed < minimum || parsed > maximum)
		return false;
	value = parsed;
	return true;
}

using ::webcool::ai::record_codec::split_tabs;

bool parse_audit_record(const std::string &line, long long &timestamp,
			std::string &event, sandbox_run_history_t &run)
{
	std::vector<std::string> fields;
	split_tabs(line, fields);
	// V1 originally had 11 fields. Field 12 was appended for cancellation so
	// the prefix remains readable by old installations and old files remain
	// readable by this version.
	if (fields.size() != 11 && fields.size() != 12)
		return false;
	long long exit_code = 0;
	long long signal = 0;
	long long elapsed = 0;
	long long timed_out = 0;
	long long truncated = 0;
	long long cancelled = 0;
	if (!parse_integer(fields[0], 0, LLONG_MAX, timestamp) ||
	    (fields[1] != "started" && fields[1] != "finished") ||
	    !valid_id(fields[2]) || !hex_decode(fields[3], run.project_path) ||
	    !hex_decode(fields[4], run.command_id) ||
	    !parse_integer(fields[5], -1, INT_MAX, exit_code) ||
	    !parse_integer(fields[6], 0, INT_MAX, signal) ||
	    !parse_integer(fields[7], 0, LLONG_MAX, elapsed) ||
	    !parse_integer(fields[8], 0, 1, timed_out) ||
	    !parse_integer(fields[9], 0, 1, truncated) ||
	    !hex_decode(fields[10], run.error) ||
	    (fields.size() == 12 &&
	     !parse_integer(fields[11], 0, 1, cancelled)))
		return false;
	event = fields[1];
	run.id = fields[2];
	run.exit_code = static_cast<int>(exit_code);
	run.signal = static_cast<int>(signal);
	run.elapsed_ms = static_cast<unsigned long long>(elapsed);
	run.timed_out = timed_out != 0;
	run.output_truncated = truncated != 0;
	run.cancelled = cancelled != 0;
	return true;
}

bool newer_history(const sandbox_run_history_t &left,
		   const sandbox_run_history_t &right)
{
	const long long left_time =
		left.finished_at > 0 ? left.finished_at : left.started_at;
	const long long right_time =
		right.finished_at > 0 ? right.finished_at : right.started_at;
	return left_time > right_time;
}

} // namespace

sandbox_execution_store_t::sandbox_execution_store_t(
	const std::string &user_root)
	: user_root_(user_root)
{
}

bool sandbox_execution_store_t::create(const std::string &project_path,
				       const std::string &command_id,
				       sandbox_execution_plan_t &plan,
				       std::string &err) const
{
	std::string normalized;
	if (!agent_workspace_t::normalize_path(project_path, normalized, true,
					       err)) {
		return ai_error("sandbox.plan", "normalize-project", err);
	}
	project_toolchain_catalog_t catalog(user_root_);
	project_toolchain_t toolchain;
	if (!catalog.discover(normalized, toolchain, err)) {
		return ai_error("sandbox.plan", "discover-toolchain", err);
	}
	const sandbox_command_t *selected = NULL;
	for (size_t i = 0; i < toolchain.commands.size(); ++i) {
		if (toolchain.commands[i].id == command_id) {
			selected = &toolchain.commands[i];
			break;
		}
	}
	if (selected == NULL || selected->allow_dynamic_arguments ||
	    selected->allow_loopback_network) {
		err = "sandbox command is not enabled by the fixed toolchain policy";
		return ai_error("sandbox.plan", "select-fixed-command", err);
	}
	plan = sandbox_execution_plan_t();
	plan.id = new_id();
	plan.project_path = normalized;
	plan.command = *selected;
	plan.limits = fixed_limits();
	plan.created_at = static_cast<long long>(time(NULL));
	if (plan.id.empty()) {
		err = "cannot initialize sandbox execution plan";
		return ai_error("sandbox.plan", "generate-id", err);
	}
	std::lock_guard<webcool::mutex> guard(g_execution_store_mutex);
	std::string directory;
	if (!ensure_store_directory(user_root_, directory, err)) {
		return ai_error("sandbox.plan", "prepare-private-store", err);
	}
	if (!save_plan(directory, plan, err)) {
		return ai_error("sandbox.plan", "save", err);
	}
	return true;
}

bool sandbox_execution_store_t::consume(const std::string &plan_id,
					sandbox_execution_plan_t &plan,
					std::string &err) const
{
	std::lock_guard<webcool::mutex> guard(g_execution_store_mutex);
	std::string directory;
	if (!ensure_store_directory(user_root_, directory, err)) {
		return ai_error("sandbox.plan", "prepare-consume-store", err);
	}
	const std::string pending = pending_path(directory);
	if (!load_plan_file(pending, plan_id, plan, err)) {
		return ai_error("sandbox.plan", "load-for-consume", err);
	}
	// Rename before any expensive revalidation. This is the one-time consume
	// point: another request can no longer load the pending authorization.
	const std::string consumed =
		join_path(directory, "consumed-" + plan_id);
	if (!move_file(pending, consumed)) {
		err = "sandbox execution plan was already consumed";
		return ai_error("sandbox.plan", "atomic-consume", err);
	}
	const long long now = static_cast<long long>(time(NULL));
	if (plan.created_at <= 0 || now < plan.created_at ||
	    now - plan.created_at > kPlanLifetimeSeconds) {
		remove(consumed.c_str());
		err = "sandbox execution plan expired";
		return ai_error("sandbox.plan", "check-expiry", err);
	}
	project_toolchain_catalog_t catalog(user_root_);
	project_toolchain_t current;
	if (!catalog.discover(plan.project_path, current, err)) {
		remove(consumed.c_str());
		return ai_error("sandbox.plan", "rediscover-toolchain", err);
	}
	bool matched = false;
	for (size_t i = 0; i < current.commands.size(); ++i) {
		if (same_command(plan.command, current.commands[i])) {
			matched = true;
			break;
		}
	}
	remove(consumed.c_str());
	if (!matched) {
		err = "sandbox toolchain changed after plan preview";
		return ai_error("sandbox.plan", "compare-command-snapshot",
				err);
	}
	return true;
}

bool sandbox_execution_store_t::audit_started(
	const sandbox_execution_plan_t &plan, std::string &err) const
{
	std::lock_guard<webcool::mutex> guard(g_execution_store_mutex);
	if (!append_audit(user_root_, plan, "started", NULL, err)) {
		return ai_error("sandbox.audit", "record-start", err);
	}
	return true;
}

bool sandbox_execution_store_t::audit_finished(
	const sandbox_execution_plan_t &plan, const sandbox_result_t &result,
	std::string &err) const
{
	std::lock_guard<webcool::mutex> guard(g_execution_store_mutex);
	if (!append_audit(user_root_, plan, "finished", &result, err)) {
		return ai_error("sandbox.audit", "record-finish", err);
	}
	return true;
}

bool sandbox_execution_store_t::list_history(
	size_t limit, std::vector<sandbox_run_history_t> &runs,
	std::string &err) const
{
	runs.clear();
	if (limit < 1 || limit > kMaxAuditRecords) {
		err = "sandbox history limit must be between 1 and 100";
		return ai_error("sandbox.audit", "validate-history-limit", err);
	}
	std::lock_guard<webcool::mutex> guard(g_execution_store_mutex);
	const std::string path = join_path(
		join_path(user_root_, ".webcool_agent"), "sandbox_runs.v1");
	std::ifstream in(path.c_str(), std::ios::in | std::ios::binary);
	if (!in.good())
		return true; // A user who has never run a command has no history.
	std::string line;
	if (!std::getline(in, line) || line != kAuditHeader) {
		err = "invalid sandbox execution audit header";
		return ai_error("sandbox.audit", "read-history-header", err);
	}
	std::map<std::string, size_t> positions;
	size_t record_count = 0;
	while (std::getline(in, line)) {
		if (line.empty())
			continue;
		if (++record_count > kMaxAuditRecords) {
			err = "sandbox execution audit exceeds the record limit";
			return ai_error("sandbox.audit", "read-history-size",
					err);
		}
		long long timestamp = 0;
		std::string event;
		sandbox_run_history_t parsed;
		if (!parse_audit_record(line, timestamp, event, parsed)) {
			err = "invalid sandbox execution audit record";
			return ai_error("sandbox.audit", "parse-history-record",
					err);
		}
		std::map<std::string, size_t>::iterator position =
			positions.find(parsed.id);
		if (position == positions.end()) {
			positions[parsed.id] = runs.size();
			runs.push_back(parsed);
			position = positions.find(parsed.id);
		}
		sandbox_run_history_t &current = runs[position->second];
		if (event == "started") {
			current.project_path = parsed.project_path;
			current.command_id = parsed.command_id;
			current.started_at = timestamp;
			current.status = "interrupted";
		} else {
			const long long started_at = current.started_at;
			current = parsed;
			current.started_at = started_at;
			current.finished_at = timestamp;
			if (current.cancelled)
				current.status = "cancelled";
			else if (current.timed_out ||
				 current.output_truncated ||
				 !current.error.empty())
				current.status = "failed";
			else
				current.status = "completed";
		}
	}
	std::sort(runs.begin(), runs.end(), newer_history);
	if (runs.size() > limit)
		runs.resize(limit);
	return true;
}

long long sandbox_execution_store_t::lifetime_seconds()
{
	return kPlanLifetimeSeconds;
}

} // namespace ai
} // namespace webcool
