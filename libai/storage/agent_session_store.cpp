#include "stdafx.h"
#include "../common/file_ops.h"
#include "../common/identifiers.h"
#include "../common/record_codec.h"
#include "agent_session_store.h"
#include "../common/ai_error_log.h"
#include "../common/webcool_mutex.h"

#ifdef _WIN32
#include "../common/platform_compat.h"
#else
#include <unistd.h>
#endif

#include <openssl/rand.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sys/stat.h>

namespace webcool {
namespace ai {
namespace {

webcool::mutex g_session_store_mutex;
const char* kLegacyHeader = "WEBCOOL_AGENT_SESSIONS_V1";
const char* kMessageHistoryHeader = "WEBCOOL_AGENT_SESSIONS_V2";
const char* kReasoningHistoryHeader = "WEBCOOL_AGENT_SESSIONS_V3";
const char* kUsageHistoryHeader = "WEBCOOL_AGENT_SESSIONS_V4";
const char* kTitleHistoryHeader = "WEBCOOL_AGENT_SESSIONS_V5";
const char* kHeader = "WEBCOOL_AGENT_SESSIONS_V6";
const char* kFile = ".webcool_agent/sessions.v1";
const size_t kMaxSessions = 50;
const size_t kMaxSummaryBytes = 8 * 1024;
const size_t kMaxMessages = 80;
const size_t kMaxMessageBytes = 512 * 1024;
const size_t kMaxReasoningBytes = 1024 * 1024;
const size_t kMaxCompletionSummaryBytes = 4 * 1024;
const size_t kMaxTranscriptBytes = 8 * 1024 * 1024;

using ::webcool::ai::file_ops::join_path;

using ::webcool::ai::record_codec::hex_value;

using ::webcool::ai::record_codec::hex_encode;

using ::webcool::ai::record_codec::hex_decode;

using ::webcool::ai::record_codec::split_tabs;

bool parse_number(const std::string& text, long long& value) {
    return ::webcool::ai::record_codec::parse_nonnegative_number(text, value);
}

using ::webcool::ai::identifiers::valid_id;

bool safe_message(const agent_session_message_t& message) {
	return (message.role == "user" || message.role == "assistant")
		&& !message.text.empty() && message.text.size() <= kMaxMessageBytes
		&& message.reasoning.size() <= kMaxReasoningBytes
		&& message.completion_summary.size() <= kMaxCompletionSummaryBytes
		&& valid_id(message.run_id)
		&& (message.state == "sent" || message.state == "completed"
			|| message.state == "failed" || message.state == "cancelled")
		&& message.created_at > 0 && message.duration_ms >= 0
		&& message.input_tokens >= 0 && message.cached_input_tokens >= 0
		&& message.output_tokens >= 0 && message.reasoning_tokens >= 0;
}

size_t transcript_bytes(const std::vector<agent_session_message_t>& messages) {
	size_t bytes = 0;
	for (size_t i = 0; i < messages.size(); ++i) {
		bytes += messages[i].text.size() + messages[i].reasoning.size()
			+ messages[i].completion_summary.size();
	}
	return bytes;
}

std::string bounded_message_text(const std::string& text) {
	if (text.size() <= kMaxMessageBytes) return text;
	const char* suffix = "\n[...历史消息过长，已截断...]";
	size_t keep = kMaxMessageBytes - strlen(suffix);
	// Do not split a UTF-8 continuation byte at the truncation boundary.
	while (keep > 0
		&& (static_cast<unsigned char>(text[keep]) & 0xc0) == 0x80) --keep;
	return text.substr(0, keep) + suffix;
}

std::string bounded_reasoning_text(const std::string& text) {
	if (text.size() <= kMaxReasoningBytes) return text;
	const char* suffix = "\n[...推理过程过长，已截断...]";
	size_t keep = kMaxReasoningBytes - strlen(suffix);
	while (keep > 0
		&& (static_cast<unsigned char>(text[keep]) & 0xc0) == 0x80) --keep;
	return text.substr(0, keep) + suffix;
}

void trim_messages(std::vector<agent_session_message_t>& messages) {
	while (messages.size() > kMaxMessages
		|| transcript_bytes(messages) > kMaxTranscriptBytes)
	{
		if (messages.empty()) return;
		const std::string oldest_run = messages.front().run_id;
		messages.erase(std::remove_if(messages.begin(), messages.end(),
			[&oldest_run](const agent_session_message_t& message) {
				return message.run_id == oldest_run;
			}), messages.end());
	}
}

std::string serialize_messages(
	const std::vector<agent_session_message_t>& messages)
{
	std::string output;
	for (size_t i = 0; i < messages.size(); ++i) {
		const agent_session_message_t& message = messages[i];
		output += message.role + "\t" + message.run_id + "\t" + message.state
			+ "\t" + std::to_string(message.created_at) + "\t"
			+ std::to_string(message.text.size()) + "\t"
			+ std::to_string(message.reasoning.size()) + "\t"
			+ std::to_string(message.completion_summary.size()) + "\t"
			+ std::to_string(message.duration_ms) + "\t"
			+ std::to_string(message.input_tokens) + "\t"
			+ std::to_string(message.cached_input_tokens) + "\t"
			+ std::to_string(message.output_tokens) + "\t"
			+ std::to_string(message.reasoning_tokens) + "\n";
		output.append(message.text);
		output.push_back('\n');
		output.append(message.reasoning);
		output.push_back('\n');
		output.append(message.completion_summary);
		output.push_back('\n');
	}
	return output;
}

bool parse_messages(const std::string& input, int format_version,
	std::vector<agent_session_message_t>& messages)
{
	messages.clear();
	size_t cursor = 0;
	while (cursor < input.size()) {
		const size_t header_end = input.find('\n', cursor);
		if (header_end == std::string::npos) return false;
		std::vector<std::string> fields;
		split_tabs(input.substr(cursor, header_end - cursor), fields);
		const bool includes_reasoning = format_version >= 3;
		const bool includes_usage = format_version >= 4;
		const bool includes_completion_summary = format_version >= 6;
		if (fields.size() != (includes_completion_summary ? 12U : includes_usage ? 11U
			: (includes_reasoning ? 7U : 5U))) return false;
		agent_session_message_t message;
		long long text_size = 0;
		long long reasoning_size = 0;
		long long completion_summary_size = 0;
		const size_t duration_index = includes_completion_summary ? 7 : 6;
		const size_t usage_index = includes_completion_summary ? 8 : 7;
		if (!parse_number(fields[3], message.created_at)
			|| !parse_number(fields[4], text_size)
			|| (includes_reasoning
				&& (!parse_number(fields[5], reasoning_size)
					|| !parse_number(fields[duration_index], message.duration_ms)))
			|| (includes_completion_summary
				&& !parse_number(fields[6], completion_summary_size))
			|| (includes_usage
				&& (!parse_number(fields[usage_index], message.input_tokens)
					|| !parse_number(fields[usage_index + 1], message.cached_input_tokens)
					|| !parse_number(fields[usage_index + 2], message.output_tokens)
					|| !parse_number(fields[usage_index + 3], message.reasoning_tokens)))
			|| text_size <= 0
			|| static_cast<unsigned long long>(text_size) > kMaxMessageBytes
			|| reasoning_size < 0
			|| static_cast<unsigned long long>(reasoning_size) > kMaxReasoningBytes
			|| completion_summary_size < 0
			|| static_cast<unsigned long long>(completion_summary_size)
				> kMaxCompletionSummaryBytes)
		{
			return false;
		}
		message.role = fields[0];
		message.run_id = fields[1];
		message.state = fields[2];
		const size_t text_begin = header_end + 1;
		const size_t size = static_cast<size_t>(text_size);
		if (size > input.size() - text_begin
			|| text_begin + size >= input.size()
			|| input[text_begin + size] != '\n') return false;
		message.text.assign(input, text_begin, size);
		const size_t reasoning_begin = text_begin + size + 1;
		if (includes_reasoning) {
			const size_t reasoning_bytes = static_cast<size_t>(reasoning_size);
			if (reasoning_bytes > input.size() - reasoning_begin
				|| reasoning_begin + reasoning_bytes >= input.size()
				|| input[reasoning_begin + reasoning_bytes] != '\n') return false;
			message.reasoning.assign(input, reasoning_begin, reasoning_bytes);
		}
		const size_t completion_summary_begin = reasoning_begin
			+ static_cast<size_t>(reasoning_size) + 1;
		if (includes_completion_summary) {
			const size_t summary_bytes = static_cast<size_t>(completion_summary_size);
			if (summary_bytes > input.size() - completion_summary_begin
				|| completion_summary_begin + summary_bytes >= input.size()
				|| input[completion_summary_begin + summary_bytes] != '\n') return false;
			message.completion_summary.assign(input, completion_summary_begin,
				summary_bytes);
		}
		if (!safe_message(message)) return false;
		messages.push_back(message);
		cursor = includes_completion_summary
			? completion_summary_begin
				+ static_cast<size_t>(completion_summary_size) + 1
			: includes_reasoning
			? reasoning_begin + static_cast<size_t>(reasoning_size) + 1
			: text_begin + size + 1;
	}
	return messages.size() <= kMaxMessages
		&& transcript_bytes(messages) <= kMaxTranscriptBytes;
}

using ::webcool::ai::identifiers::new_id;

bool safe_record(const agent_session_record_t& record) {
	return valid_id(record.id) && !record.title.empty()
		&& record.title.size() <= 120 && !record.agent_id.empty()
		&& record.agent_id.size() <= 64 && record.provider_id.size() <= 64
		&& record.project_path.size() <= 2048
		&& record.summary.size() <= kMaxSummaryBytes
		&& record.messages.size() <= kMaxMessages
		&& transcript_bytes(record.messages) <= kMaxTranscriptBytes
		&& (record.last_run_id.empty() || valid_id(record.last_run_id))
		&& record.created_at > 0 && record.updated_at >= record.created_at
		&& record.turn_count >= 0
		&& std::all_of(record.messages.begin(), record.messages.end(), safe_message);
}

bool ensure_directory(const std::string& user_root, std::string& err) {
	const std::string directory = join_path(user_root, ".webcool_agent");
#ifdef _WIN32
	std::wstring wide;
	if (!webcool_utf8_path_to_wide(directory.c_str(), wide)) {
		err = "cannot validate agent session directory";
		return false;
	}
	DWORD attributes = GetFileAttributesW(wide.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES) {
		if (_wmkdir(wide.c_str()) != 0) {
			err = "cannot create agent session directory";
			return false;
		}
		attributes = GetFileAttributesW(wide.c_str());
	}
	if (attributes == INVALID_FILE_ATTRIBUTES
		|| (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0
		|| (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
	{
		err = "agent session path is not a safe directory";
		return false;
	}
#else
	struct stat st;
	if (lstat(directory.c_str(), &st) != 0) {
		if (mkdir(directory.c_str(), 0700) != 0
			|| lstat(directory.c_str(), &st) != 0)
		{
			err = "cannot create agent session directory";
			return false;
		}
	}
	if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)
		|| chmod(directory.c_str(), 0700) != 0)
	{
		err = "agent session path is not a safe private directory";
		return false;
	}
#endif
	return true;
}

using ::webcool::ai::file_ops::replace_file;

bool load_records(const std::string& user_root,
	std::vector<agent_session_record_t>& records, std::string& err)
{
	records.clear();
	std::ifstream in(join_path(user_root, kFile).c_str(),
		std::ios::in | std::ios::binary);
	if (!in.good()) return true;
	std::string line;
	if (!std::getline(in, line)
		|| (line != kHeader && line != kTitleHistoryHeader
			&& line != kUsageHistoryHeader && line != kMessageHistoryHeader
			&& line != kReasoningHistoryHeader && line != kLegacyHeader)) {
		err = "invalid agent session database";
		return false;
	}
	const bool legacy = line == kLegacyHeader;
	const bool migrate_titles = line != kHeader;
	const int format_version = line == kHeader ? 6
		: (line == kTitleHistoryHeader ? 5
		: line == kUsageHistoryHeader ? 4
		: (line == kReasoningHistoryHeader ? 3
			: (line == kMessageHistoryHeader ? 2 : 1)));
	while (std::getline(in, line)) {
		if (line.empty()) continue;
		std::vector<std::string> fields;
		split_tabs(line, fields);
		if (fields.size() != (legacy ? 10U : 11U)) {
			err = "invalid agent session record";
			return false;
		}
		agent_session_record_t record;
		std::string serialized_messages;
		if (!hex_decode(fields[0], record.id)
			|| !hex_decode(fields[1], record.title)
			|| !hex_decode(fields[2], record.agent_id)
			|| !hex_decode(fields[3], record.provider_id)
			|| !hex_decode(fields[4], record.project_path)
			|| !hex_decode(fields[5], record.summary)
			|| (!legacy && (!hex_decode(fields[6], serialized_messages)
				|| !parse_messages(serialized_messages, format_version,
					record.messages)))
			|| !hex_decode(fields[legacy ? 6 : 7], record.last_run_id)
			|| !parse_number(fields[legacy ? 7 : 8], record.created_at)
			|| !parse_number(fields[legacy ? 8 : 9], record.updated_at)
			|| !parse_number(fields[legacy ? 9 : 10], record.turn_count)
			|| !safe_record(record))
		{
			err = "invalid agent session record";
			return false;
		}
		// Upgrade legacy generated titles only when the complete turn history is
		// still present. A trimmed transcript must not rename a session after a
		// later request. No write is needed merely to display the upgraded title.
		size_t user_messages = 0;
		for (const auto& message : record.messages)
			if (message.role == "user") ++user_messages;
		if (migrate_titles && !record.messages.empty() && record.messages.front().role == "user"
			&& user_messages >= static_cast<size_t>(record.turn_count))
			record.title = agent_session_task_title(record.messages.front().text);
		records.push_back(record);
		if (records.size() > kMaxSessions) {
			err = "agent session database exceeds its limit";
			return false;
		}
	}
	return true;
}

bool save_records(const std::string& user_root,
	const std::vector<agent_session_record_t>& records, std::string& err)
{
	if (!ensure_directory(user_root, err)) return false;
	const std::string path = join_path(user_root, kFile);
	const std::string temporary = path + ".tmp";
	std::ofstream out(temporary.c_str(), std::ios::out | std::ios::binary
		| std::ios::trunc);
	if (!out.good()) {
		err = "cannot write agent session database";
		return false;
	}
	out << kHeader << '\n';
	for (size_t i = 0; i < records.size(); ++i) {
		if (!safe_record(records[i])) {
			out.close();
			remove(temporary.c_str());
			err = "invalid agent session record";
			return false;
		}
		out << hex_encode(records[i].id) << '\t'
			<< hex_encode(records[i].title) << '\t'
			<< hex_encode(records[i].agent_id) << '\t'
			<< hex_encode(records[i].provider_id) << '\t'
			<< hex_encode(records[i].project_path) << '\t'
			<< hex_encode(records[i].summary) << '\t'
			<< hex_encode(serialize_messages(records[i].messages)) << '\t'
			<< hex_encode(records[i].last_run_id) << '\t'
			<< records[i].created_at << '\t' << records[i].updated_at << '\t'
			<< records[i].turn_count << '\n';
	}
	out.close();
	if (!out.good()) {
		err = "cannot flush agent session database";
		return false;
	}
#ifndef _WIN32
	if (chmod(temporary.c_str(), 0600) != 0) {
		unlink(temporary.c_str());
		err = "cannot protect agent session database";
		return false;
	}
#endif
	if (!replace_file(temporary, path)) {
		remove(temporary.c_str());
		err = std::string("cannot install agent session database: ")
			+ strerror(errno);
		return false;
	}
	return true;
}

bool newer_session(const agent_session_record_t& left,
	const agent_session_record_t& right)
{
	if (left.updated_at != right.updated_at) return left.updated_at > right.updated_at;
	return left.id > right.id;
}

} // namespace

std::string agent_session_task_title(const std::string& prompt) {
	const size_t begin = prompt.find_first_not_of(" \t\r\n");
	if (begin == std::string::npos) return "编程会话";
	size_t end = prompt.size();
	for (const char* separator : {"。", "！", "？", "!", "?", "\n", "\r"}) {
		const size_t pos = prompt.find(separator, begin);
		if (pos != std::string::npos) end = std::min(end, pos);
	}
	// A dot inside filenames/versions is not an English sentence boundary.
	for (size_t pos = prompt.find('.', begin); pos != std::string::npos;
		pos = prompt.find('.', pos + 1)) {
		if (pos + 1 == prompt.size() || prompt[pos + 1] == ' '
			|| prompt[pos + 1] == '\n' || prompt[pos + 1] == '\r') {
			end = std::min(end, pos); break;
		}
	}
	if (end == begin) end = prompt.size();
	const bool shortened = end - begin > 120;
	if (shortened) {
		end = begin + 117; // Reserve three UTF-8 bytes for the ellipsis.
		while (end > begin && (static_cast<unsigned char>(prompt[end]) & 0xc0) == 0x80) --end;
	}
	std::string title = prompt.substr(begin, end - begin);
	while (!title.empty() && (title.back() == ' ' || title.back() == '\t')) title.pop_back();
	if (shortened) title += "…";
	return title.empty() ? "编程会话" : title;
}

agent_session_store_t::agent_session_store_t(const std::string& user_root)
	: user_root_(user_root) {}

bool agent_session_store_t::create(const std::string& title,
	const std::string& agent_id, const std::string& provider_id,
	const std::string& project_path, agent_session_record_t& record,
	std::string& err) const
{
	if (title.empty() || title.size() > 120 || agent_id.empty()
		|| agent_id.size() > 64 || provider_id.size() > 64
		|| project_path.size() > 2048)
	{
		err = "invalid agent session settings";
		return ai_error("agent.session-store", "validate-create", err);
	}
	std::lock_guard<webcool::mutex> guard(g_session_store_mutex);
	std::vector<agent_session_record_t> records;
	if (!load_records(user_root_, records, err)) {
		return ai_error("agent.session-store", "load-for-create", err);
	}
	record = agent_session_record_t();
	record.id = new_id();
	if (record.id.empty()) {
		err = "cannot generate agent session id";
		return ai_error("agent.session-store", "generate-id", err);
	}
	record.title = title;
	record.agent_id = agent_id;
	record.provider_id = provider_id;
	record.project_path = project_path;
	record.created_at = static_cast<long long>(time(NULL));
	record.updated_at = record.created_at;
	records.push_back(record);
	std::sort(records.begin(), records.end(), newer_session);
	if (records.size() > kMaxSessions) records.resize(kMaxSessions);
	if (!save_records(user_root_, records, err)) {
		return ai_error("agent.session-store", "save-create", err);
	}
	return true;
}

bool agent_session_store_t::update_after_run(const std::string& id,
	const std::string& title, const std::string& summary,
	const std::string& last_run_id, const std::string& user_prompt,
	const std::string& assistant_reply, const std::string& assistant_state,
	const std::string& assistant_reasoning,
	const std::string& completion_summary, long long duration_ms,
	long long input_tokens, long long cached_input_tokens,
	long long output_tokens, long long reasoning_tokens,
	std::string& err) const
{
	const bool valid_state = assistant_state == "completed"
		|| assistant_state == "failed" || assistant_state == "cancelled";
	if (!valid_id(id) || !valid_id(last_run_id)
		|| title.empty() || title.size() > 120
		|| summary.size() > kMaxSummaryBytes
		|| completion_summary.size() > kMaxCompletionSummaryBytes
		|| user_prompt.empty() || assistant_reply.empty() || !valid_state
		|| duration_ms < 0 || input_tokens < 0 || cached_input_tokens < 0
		|| output_tokens < 0 || reasoning_tokens < 0)
	{
		err = "invalid agent session update";
		return ai_error("agent.session-store", "validate-update", err);
	}
	std::lock_guard<webcool::mutex> guard(g_session_store_mutex);
	std::vector<agent_session_record_t> records;
	if (!load_records(user_root_, records, err)) {
		return ai_error("agent.session-store", "load-for-update", err);
	}
	for (size_t i = 0; i < records.size(); ++i) {
		if (records[i].id != id) continue;
		// A conversation is named by its first completed request. Later turns may
		// produce a new model-generated session_title, but replacing the stored
		// title would make the same topic appear to change identity in the picker.
		// Requiring all durable run fields to be empty also preserves titles from
		// legacy summary-only sessions when they receive their first new message.
		const bool first_completed_run = records[i].turn_count == 0
			&& records[i].last_run_id.empty()
			&& records[i].messages.empty()
			&& records[i].summary.empty();
		if (first_completed_run) records[i].title = agent_session_task_title(user_prompt);
		if (!summary.empty()) records[i].summary = summary;
		records[i].last_run_id = last_run_id;
		records[i].updated_at = static_cast<long long>(time(NULL));
		// One completed run contributes exactly one user/assistant pair. Recovery
		// retries use the same run id, so replace that pair instead of duplicating it.
		records[i].messages.erase(std::remove_if(records[i].messages.begin(),
			records[i].messages.end(), [&last_run_id](
				const agent_session_message_t& message) {
				return message.run_id == last_run_id;
			}), records[i].messages.end());
		agent_session_message_t user_message;
		user_message.role = "user";
		user_message.text = bounded_message_text(user_prompt);
		user_message.run_id = last_run_id;
		user_message.state = "sent";
		user_message.created_at = records[i].updated_at;
		records[i].messages.push_back(user_message);
		agent_session_message_t assistant_message;
		assistant_message.role = "assistant";
		assistant_message.text = bounded_message_text(assistant_reply);
		assistant_message.reasoning = bounded_reasoning_text(assistant_reasoning);
		assistant_message.completion_summary = completion_summary;
		assistant_message.run_id = last_run_id;
		assistant_message.state = assistant_state;
		assistant_message.created_at = records[i].updated_at;
		assistant_message.duration_ms = duration_ms;
		assistant_message.input_tokens = input_tokens;
		assistant_message.cached_input_tokens = cached_input_tokens;
		assistant_message.output_tokens = output_tokens;
		assistant_message.reasoning_tokens = reasoning_tokens;
		records[i].messages.push_back(assistant_message);
		trim_messages(records[i].messages);
		++records[i].turn_count;
		std::sort(records.begin(), records.end(), newer_session);
		if (!save_records(user_root_, records, err)) {
			return ai_error("agent.session-store", "save-update", err);
		}
		return true;
	}
	err = "agent session not found";
	return ai_error("agent.session-store", "find-for-update", err);
}

bool agent_session_store_t::get(const std::string& id,
	agent_session_record_t& record, std::string& err) const
{
	if (!valid_id(id)) {
		err = "invalid agent session id";
		return ai_error("agent.session-store", "validate-get", err);
	}
	std::lock_guard<webcool::mutex> guard(g_session_store_mutex);
	std::vector<agent_session_record_t> records;
	if (!load_records(user_root_, records, err)) {
		return ai_error("agent.session-store", "load-for-get", err);
	}
	for (size_t i = 0; i < records.size(); ++i) {
		if (records[i].id == id) {
			record = records[i];
			return true;
		}
	}
	err = "agent session not found";
	return ai_error("agent.session-store", "find-for-get", err);
}

bool agent_session_store_t::list(size_t limit,
	std::vector<agent_session_record_t>& records, std::string& err) const
{
	if (limit < 1 || limit > kMaxSessions) {
		err = "agent session list limit is invalid";
		return ai_error("agent.session-store", "validate-list", err);
	}
	std::lock_guard<webcool::mutex> guard(g_session_store_mutex);
	if (!load_records(user_root_, records, err)) {
		return ai_error("agent.session-store", "load-list", err);
	}
	std::sort(records.begin(), records.end(), newer_session);
	if (records.size() > limit) records.resize(limit);
	return true;
}

bool agent_session_store_t::list_for_project(const std::string& project_path,
	const std::string& exclude_session_id, size_t limit,
	std::vector<agent_session_record_t>& records, std::string& err) const
{
	if (project_path.size() > 2048 || limit < 1 || limit > kMaxSessions
		|| (!exclude_session_id.empty() && !valid_id(exclude_session_id)))
	{
		err = "invalid project-memory query";
		return ai_error("agent.session-store", "validate-project-memory", err);
	}
	std::lock_guard<webcool::mutex> guard(g_session_store_mutex);
	std::vector<agent_session_record_t> all;
	if (!load_records(user_root_, all, err)) {
		return ai_error("agent.session-store", "load-project-memory", err);
	}
	std::sort(all.begin(), all.end(), newer_session);
	records.clear();
	for (size_t i = 0; i < all.size() && records.size() < limit; ++i) {
		if (all[i].project_path == project_path
			&& all[i].id != exclude_session_id && !all[i].summary.empty())
		{
			records.push_back(all[i]);
		}
	}
	return true;
}

bool agent_session_store_t::remove(const std::string& id,
	std::string& err) const
{
	if (!valid_id(id)) {
		err = "invalid agent session id";
		return ai_error("agent.session-store", "validate-remove", err);
	}
	std::lock_guard<webcool::mutex> guard(g_session_store_mutex);
	std::vector<agent_session_record_t> records;
	if (!load_records(user_root_, records, err)) {
		return ai_error("agent.session-store", "load-for-remove", err);
	}
	const size_t before = records.size();
	records.erase(std::remove_if(records.begin(), records.end(),
		[&id](const agent_session_record_t& record) { return record.id == id; }),
		records.end());
	if (records.size() == before) {
		err = "agent session not found";
		return ai_error("agent.session-store", "find-for-remove", err);
	}
	if (!save_records(user_root_, records, err)) {
		return ai_error("agent.session-store", "save-remove", err);
	}
	return true;
}

bool agent_session_store_t::remove_for_project(const std::string& project_path,
	size_t& removed_count, std::string& err) const
{
	removed_count = 0;
	if (project_path.empty() || project_path.size() > 2048) {
		err = "invalid agent session project path";
		return ai_error("agent.session-store", "validate-remove-project", err);
	}
	std::lock_guard<webcool::mutex> guard(g_session_store_mutex);
	std::vector<agent_session_record_t> records;
	if (!load_records(user_root_, records, err)) {
		return ai_error("agent.session-store", "load-for-remove-project", err);
	}
	const size_t before = records.size();
	records.erase(std::remove_if(records.begin(), records.end(),
		[&project_path](const agent_session_record_t& record) {
			return record.project_path == project_path;
		}), records.end());
	removed_count = before - records.size();
	if (removed_count == 0) return true;
	if (!save_records(user_root_, records, err)) {
		removed_count = 0;
		return ai_error("agent.session-store", "save-remove-project", err);
	}
	return true;
}

} // namespace ai
} // namespace webcool
