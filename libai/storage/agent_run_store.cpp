#include "stdafx.h"
#include "../common/file_ops.h"
#include "../common/identifiers.h"
#include "../common/record_codec.h"
#include "agent_run_store.h"
#include "../common/ai_error_log.h"
#include "../common/webcool_mutex.h"

#ifdef _WIN32
#include "../common/platform_compat.h"
#include <direct.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace webcool
{
namespace ai
{
namespace
{

// runs.v1 is a private, versioned, line-oriented metadata file. Text fields are
// hex encoded so tabs/newlines cannot corrupt record boundaries. The entire
// bounded file is rewritten through a 0600 temporary file and atomic rename.
webcool::mutex g_run_store_mutex;
const char *kHeaderV1 = "WEBCOOL_AGENT_RUNS_V1";
const char *kHeaderV2 = "WEBCOOL_AGENT_RUNS_V2";
const char *kRunFile = ".webcool_agent/runs.v1";
const size_t kMaxStoredRuns = 100;
const size_t kMaxErrorBytes = 2000;

using ::webcool::ai::file_ops::join_path;

using ::webcool::ai::record_codec::hex_value;

using ::webcool::ai::identifiers::valid_id;

using ::webcool::ai::record_codec::hex_encode;

using ::webcool::ai::record_codec::hex_decode;

using ::webcool::ai::record_codec::split_tabs;

bool parse_number(const std::string &value, long long &number)
{
	char *end = NULL;
	errno = 0;
	const long long parsed = strtoll(value.c_str(), &end, 10);
	if (errno != 0 || end == value.c_str() || *end != '\0')
		return false;
	number = parsed;
	return true;
}

bool valid_status(const std::string &status)
{
	return status == "running" || status == "completed" ||
	    status == "failed" || status == "cancelled";
}

bool safe_record(const agent_run_record_t &record)
{
	return valid_id(record.id) && !record.agent_id.empty() &&
	    record.agent_id.size() <= 64 && record.agent_version.size() <= 32 &&
	    valid_status(record.status) && record.provider_id.size() <= 64 &&
	    record.model.size() <= 256 && record.project_path.size() <= 2048 &&
	    record.started_at > 0 && record.finished_at >= 0 &&
	    record.input_tokens >= 0 && record.cached_input_tokens >= 0 &&
	    record.output_tokens >= 0 && record.reasoning_tokens >= 0 &&
	    record.latency_ms >= 0 && record.tool_calls >= 0 &&
	    record.proposed_changes >= 0 && record.rejected_changes >= 0 &&
	    record.provider_error_category.size() <= 64 &&
	    record.provider_http_status >= 0 &&
	    record.provider_http_status <= 599 &&
	    record.error.size() <= kMaxErrorBytes;
}

bool ensure_directory(const std::string &user_root, std::string &err)
{
	const std::string directory = join_path(user_root, ".webcool_agent");
#ifdef _WIN32
	std::wstring wide;
	if (!webcool_utf8_path_to_wide(directory.c_str(), wide)) {
		err = "cannot validate agent run history directory";
		return false;
	}
	DWORD attributes = GetFileAttributesW(wide.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES) {
		if (_wmkdir(wide.c_str()) != 0) {
			err = "cannot create agent run history directory";
			return false;
		}
		attributes = GetFileAttributesW(wide.c_str());
	}
	if (attributes == INVALID_FILE_ATTRIBUTES ||
	    (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
	    (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
		err = "agent run history path is not a safe directory";
		return false;
	}
#else
	struct stat st;

	if ((lstat(directory.c_str(), &st) != 0) &&
	    (mkdir(directory.c_str(), 0700) != 0 ||
	        lstat(directory.c_str(), &st) != 0)) {
		err = "cannot create agent run history directory";
		return false;
	}
	if (lstat(directory.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) ||
	    S_ISLNK(st.st_mode) || chmod(directory.c_str(), 0700) != 0) {
		err = "agent run history path is not a safe private directory";
		return false;
	}
#endif
	return true;
}

std::string store_path(const std::string &user_root)
{
	return join_path(user_root, kRunFile);
}

using ::webcool::ai::file_ops::replace_file;

bool newer_run(const agent_run_record_t &left, const agent_run_record_t &right)
{
	if (!(left.started_at != right.started_at))
		return left.id > right.id;
	return left.started_at > right.started_at;
}

bool load_records(const std::string &user_root,
    std::vector<agent_run_record_t> &records, std::string &err)
{
	records.clear();
	const std::string path = store_path(user_root);
#ifdef _WIN32
	std::wstring wide;
	if (!webcool_utf8_path_to_wide(path.c_str(), wide)) {
		err = "cannot validate agent run history";
		return false;
	}
	const DWORD attributes = GetFileAttributesW(wide.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES &&
	    GetLastError() != ERROR_FILE_NOT_FOUND &&
	    GetLastError() != ERROR_PATH_NOT_FOUND) {
		err = "cannot inspect agent run history";
		return false;
	}
	if (attributes != INVALID_FILE_ATTRIBUTES &&
	    ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
	        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)) {
		err = "agent run history is not a safe regular file";
		return false;
	}
#else
	struct stat st;
	if (lstat(path.c_str(), &st) == 0) {
		if (!S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
			err = "agent run history is not a safe regular file";
			return false;
		}
	} else if (errno != ENOENT) {
		err = "cannot inspect agent run history";
		return false;
	}
#endif
	std::ifstream in(path.c_str(), std::ios::in | std::ios::binary);
	if (!in.good())
		return true;
	std::string line;
	if (!std::getline(in, line) ||
	    (line != kHeaderV1 && line != kHeaderV2)) {
		err = "invalid agent run history header";
		return false;
	}
	const bool version2 = line == kHeaderV2;
	std::vector<std::string> fields;
	while (std::getline(in, line)) {
		if (line.empty())
			continue;
		split_tabs(line, fields);
		if (fields.size() != (version2 ? 21U : 16U)) {
			err = "invalid agent run history record";
			return false;
		}
		agent_run_record_t record;
		if (!hex_decode(fields[0], record.id) ||
		    !hex_decode(fields[1], record.agent_id) ||
		    !hex_decode(fields[2], record.agent_version) ||
		    !hex_decode(fields[3], record.status) ||
		    !hex_decode(fields[4], record.provider_id) ||
		    !hex_decode(fields[5], record.model) ||
		    !hex_decode(fields[6], record.project_path) ||
		    !parse_number(fields[7], record.started_at) ||
		    !parse_number(fields[8], record.finished_at) ||
		    !parse_number(fields[9], record.input_tokens) ||
		    !parse_number(fields[10], record.output_tokens) ||
		    !parse_number(fields[11], record.latency_ms) ||
		    !parse_number(fields[12], record.tool_calls) ||
		    !parse_number(fields[13], record.proposed_changes) ||
		    !parse_number(fields[14], record.rejected_changes) ||
		    !hex_decode(fields[15], record.error)) {
			err = "invalid agent run history record";
			return false;
		}
		if (version2) {
			long long http_status = 0;
			long long retryable = 0;
			if (!parse_number(
			        fields[16], record.cached_input_tokens) ||
			    !parse_number(
			        fields[17], record.reasoning_tokens) ||
			    !hex_decode(
			        fields[18], record.provider_error_category) ||
			    !parse_number(fields[19], http_status) ||
			    !parse_number(fields[20], retryable) ||
			    (retryable != 0 && retryable != 1)) {
				err = "invalid agent run history V2 metadata";
				return false;
			}
			record.provider_http_status =
			    static_cast<int>(http_status);
			record.provider_error_retryable = retryable == 1;
		}
		if (!safe_record(record)) {
			err = "invalid agent run history record";
			return false;
		}
		records.push_back(record);
		if (!(records.size() > kMaxStoredRuns))
			continue;
		err = "agent run history exceeds its record limit";
		return false;
	}
	return true;
}

bool save_records(const std::string &user_root,
    const std::vector<agent_run_record_t> &records, std::string &err)
{
	if (!ensure_directory(user_root, err))
		return false;
	const std::string path = store_path(user_root);
	const std::string temporary = path + ".tmp";
	std::ofstream out(temporary.c_str(),
	    std::ios::out | std::ios::binary | std::ios::trunc);
	if (!out.good()) {
		err = "cannot write agent run history";
		return false;
	}
	out << kHeaderV2 << '\n';
	for (size_t i = 0; i < records.size(); ++i) {
		if (!safe_record(records[i])) {
			out.close();
			remove(temporary.c_str());
			err = "invalid agent run history record";
			return false;
		}
		out << hex_encode(records[i].id) << '\t'
		    << hex_encode(records[i].agent_id) << '\t'
		    << hex_encode(records[i].agent_version) << '\t'
		    << hex_encode(records[i].status) << '\t'
		    << hex_encode(records[i].provider_id) << '\t'
		    << hex_encode(records[i].model) << '\t'
		    << hex_encode(records[i].project_path) << '\t'
		    << records[i].started_at << '\t' << records[i].finished_at
		    << '\t' << records[i].input_tokens << '\t'
		    << records[i].output_tokens << '\t' << records[i].latency_ms
		    << '\t' << records[i].tool_calls << '\t'
		    << records[i].proposed_changes << '\t'
		    << records[i].rejected_changes << '\t'
		    << hex_encode(records[i].error) << '\t'
		    << records[i].cached_input_tokens << '\t'
		    << records[i].reasoning_tokens << '\t'
		    << hex_encode(records[i].provider_error_category) << '\t'
		    << records[i].provider_http_status << '\t'
		    << (records[i].provider_error_retryable ? 1 : 0) << '\n';
	}
	out.close();
	if (!out.good()) {
		remove(temporary.c_str());
		err = "cannot flush agent run history";
		return false;
	}
#ifndef _WIN32
	if (chmod(temporary.c_str(), 0600) != 0) {
		remove(temporary.c_str());
		err = "cannot protect agent run history";
		return false;
	}
#endif
	if (replace_file(temporary, path))
		return true;
	remove(temporary.c_str());
	err =
	    std::string("cannot install agent run history: ") + strerror(errno);
	return false;
}

bool update_terminal(const std::string &user_root, const std::string &id,
    const std::string &status, long long input_tokens, long long output_tokens,
    long long cached_input_tokens, long long reasoning_tokens,
    long long latency_ms, long long tool_calls, long long proposed_changes,
    long long rejected_changes, const std::string &error,
    const std::string &provider_error_category, int provider_http_status,
    bool provider_error_retryable, std::string &err)
{
	if (!valid_id(id) ||
	    (status != "completed" && status != "failed" &&
	        status != "cancelled")) {
		err = "invalid agent run update";
		return false;
	}
	std::vector<agent_run_record_t> records;
	if (!load_records(user_root, records, err))
		return false;
	for (size_t i = 0; i < records.size(); ++i) {
		if (records[i].id != id)
			continue;
		if (records[i].status != "running") {
			err = "agent run is already finished";
			return false;
		}
		records[i].status = status;
		records[i].finished_at = static_cast<long long>(time(NULL));
		records[i].input_tokens = input_tokens;
		records[i].cached_input_tokens = cached_input_tokens;
		records[i].output_tokens = output_tokens;
		records[i].reasoning_tokens = reasoning_tokens;
		records[i].latency_ms = latency_ms;
		records[i].tool_calls = tool_calls;
		records[i].proposed_changes = proposed_changes;
		records[i].rejected_changes = rejected_changes;
		records[i].provider_error_category = provider_error_category;
		records[i].provider_http_status = provider_http_status;
		records[i].provider_error_retryable = provider_error_retryable;
		records[i].error = error.substr(0, kMaxErrorBytes);
		return save_records(user_root, records, err);
	}
	err = "agent run not found";
	return false;
}

} // namespace

agent_run_store_t::agent_run_store_t(const std::string &user_root)
        : user_root_(user_root)
{
}

bool agent_run_store_t::create(
    const agent_run_record_t &record, std::string &err) const
{
	if (!safe_record(record) || record.status != "running" ||
	    record.finished_at != 0) {
		err = "invalid new agent run record";
		return ai_error("agent.run-store", "validate-create", err);
	}
	std::lock_guard<webcool::mutex> guard(g_run_store_mutex);
	std::vector<agent_run_record_t> records;
	if (!load_records(user_root_, records, err)) {
		return ai_error("agent.run-store", "load-for-create", err);
	}
	for (size_t i = 0; i < records.size(); ++i) {
		if (!(records[i].id == record.id))
			continue;
		err = "agent run already exists";
		return ai_error("agent.run-store", "check-duplicate", err);
	}
	records.push_back(record);
	std::sort(records.begin(), records.end(), newer_run);
	if (records.size() > kMaxStoredRuns)
		records.resize(kMaxStoredRuns);
	if (save_records(user_root_, records, err))
		return true;
	return ai_error("agent.run-store", "save-create", err);
}

bool agent_run_store_t::complete(const std::string &id, long long input_tokens,
    long long cached_input_tokens, long long output_tokens,
    long long reasoning_tokens, long long latency_ms, long long tool_calls,
    long long proposed_changes, long long rejected_changes,
    std::string &err) const
{
	std::lock_guard<webcool::mutex> guard(g_run_store_mutex);
	if (update_terminal(user_root_, id, "completed", input_tokens,
	        output_tokens, cached_input_tokens, reasoning_tokens,
	        latency_ms, tool_calls, proposed_changes, rejected_changes, "",
	        "", 0, false, err))
		return true;
	return ai_error("agent.run-store", "complete", err);
}

bool agent_run_store_t::fail(
    const std::string &id, const std::string &error, std::string &err) const
{
	return fail(id, error, "", 0, false, err);
}

bool agent_run_store_t::fail(const std::string &id, const std::string &error,
    const std::string &provider_error_category, int provider_http_status,
    bool provider_error_retryable, std::string &err) const
{
	std::lock_guard<webcool::mutex> guard(g_run_store_mutex);
	if (update_terminal(user_root_, id, "failed", 0, 0, 0, 0, 0, 0, 0, 0,
	        error, provider_error_category, provider_http_status,
	        provider_error_retryable, err))
		return true;
	// Do not log `error`: it may originate from a model provider. Only log
	// the metadata-store failure returned through `err`.
	return ai_error("agent.run-store", "mark-failed", err);
}

bool agent_run_store_t::cancel(const std::string &id, std::string &err) const
{
	std::lock_guard<webcool::mutex> guard(g_run_store_mutex);
	if (update_terminal(user_root_, id, "cancelled", 0, 0, 0, 0, 0, 0, 0, 0,
	        "", "cancelled", 0, false, err))
		return true;
	return ai_error("agent.run-store", "cancel", err);
}

bool agent_run_store_t::get(
    const std::string &id, agent_run_record_t &record, std::string &err) const
{
	if (!valid_id(id)) {
		err = "invalid agent run id";
		return ai_error("agent.run-store", "validate-get-id", err);
	}
	std::lock_guard<webcool::mutex> guard(g_run_store_mutex);
	std::vector<agent_run_record_t> records;
	if (!load_records(user_root_, records, err)) {
		return ai_error("agent.run-store", "load-for-get", err);
	}
	for (size_t i = 0; i < records.size(); ++i) {
		if (!(records[i].id == id))
			continue;
		record = records[i];
		return true;
	}
	err = "agent run not found";
	return ai_error("agent.run-store", "find-run", err);
}

bool agent_run_store_t::list(size_t limit,
    std::vector<agent_run_record_t> &records, std::string &err) const
{
	if (limit == 0 || limit > kMaxStoredRuns)
		limit = kMaxStoredRuns;
	std::lock_guard<webcool::mutex> guard(g_run_store_mutex);
	if (!load_records(user_root_, records, err)) {
		return ai_error("agent.run-store", "list", err);
	}
	std::sort(records.begin(), records.end(), newer_run);
	if (!(records.size() > limit))
		return true;
	records.resize(limit);
	return true;
}

} // namespace ai
} // namespace webcool
