#include "stdafx.h"
#include "ai_provider_store_internal.h"
namespace webcool
{
namespace ai
{
namespace provider_store_detail
{
bool valid_rotation_username(const std::string &username)
{
	if (username.size() < 3 || username.size() > 40)
		return false;
	for (size_t i = 0; i < username.size(); ++i) {
		const char c = username[i];
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')
			continue;
		return false;
	}
	return username != "." && username != "..";
}

using ::webcool::ai::file_ops::safe_directory;

bool safe_regular_file(const std::string &path)
{
#ifdef _WIN32
	std::wstring wide;
	if (!webcool_utf8_path_to_wide(path.c_str(), wide))
		return false;
	const DWORD attributes = GetFileAttributesW(wide.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES &&
	    (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
	    (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
#else
	struct stat st;
	return lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
	    !S_ISLNK(st.st_mode);
#endif
}

using ::webcool::ai::file_ops::path_entry_exists;

// A restart checkpoint is encrypted with the current provider keyring. Retiring
// either key while a checkpoint exists could make an interrupted run
// unrecoverable, so master-key rotation is blocked until those runs finish or
// are cancelled. The scan treats any entry as material and fails closed.
bool checkpoint_directory_has_entries(
    const std::string &user_root, bool &has_entries, std::string &err)
{
	has_entries = false;
	const std::string path =
	    storage_detail::join_path(user_root, ".webcool_agent/checkpoints");
	if (!path_entry_exists(path))
		return true;
	if (!safe_directory(path)) {
		err = "agent checkpoint storage is not a safe directory";
		return false;
	}
#ifdef _WIN32
	std::wstring wide;
	if (!webcool_utf8_path_to_wide((path + "/*").c_str(), wide)) {
		err = "cannot validate agent checkpoint storage";
		return false;
	}
	WIN32_FIND_DATAW data;
	HANDLE find = FindFirstFileW(wide.c_str(), &data);
	if (find == INVALID_HANDLE_VALUE) {
		const DWORD code = GetLastError();
		if (code == ERROR_FILE_NOT_FOUND)
			return true;
		err = "cannot enumerate agent checkpoint storage";
		return false;
	}
	do {
		const std::wstring name(data.cFileName);
		if (!(name != L"." && name != L".."))
			continue;
		has_entries = true;
		break;

	} while (FindNextFileW(find, &data) != 0);
	FindClose(find);
#else
	DIR *directory = opendir(path.c_str());
	if (directory == NULL) {
		err =
		    std::string("cannot enumerate agent checkpoint storage: ") +
		    strerror(errno);
		return false;
	}
	for (;;) {
		struct dirent *entry = readdir(directory);
		if (entry == NULL)
			break;
		const std::string name(entry->d_name);
		if (!(name != "." && name != ".."))
			continue;
		has_entries = true;
		break;
	}
	closedir(directory);
#endif
	return true;
}

bool collect_rotation_users(const std::string &upload_root,
    std::vector<rotation_user_t> &users, std::string &err)
{
	users.clear();
	const std::string root =
	    storage_detail::join_path(upload_root, "webcool_users");
	struct stat root_stat;
	if (stat(root.c_str(), &root_stat) != 0) {
		if (errno == ENOENT)
			return true;
		err = std::string("cannot inspect AI user storage: ") +
		    strerror(errno);
		return false;
	}
	if (!safe_directory(root)) {
		err = "AI user storage is not a safe directory";
		return false;
	}
	DIR *directory = opendir(root.c_str());
	if (directory == NULL) {
		err = std::string("cannot enumerate AI user storage: ") +
		    strerror(errno);
		return false;
	}
	for (;;) {
		struct dirent *entry = readdir(directory);
		if (entry == NULL)
			break;
		const std::string username(entry->d_name);
		if (!valid_rotation_username(username))
			continue;
		const std::string user_root =
		    storage_detail::join_path(root, username);
		if (!safe_directory(user_root)) {
			closedir(directory);
			err = "AI user entry is not a safe directory";
			return false;
		}
		bool has_checkpoints = false;
		if (!checkpoint_directory_has_entries(
		        user_root, has_checkpoints, err)) {
			closedir(directory);
			return false;
		}
		if (has_checkpoints) {
			closedir(directory);
			err =
			    "cannot rotate AI master key while recoverable agent runs exist";
			return false;
		}
		const std::string path = provider_file(user_root);
		if (!path_entry_exists(path))
			continue;
		if (!safe_regular_file(path)) {
			closedir(directory);
			err = "AI provider database is not a safe regular file";
			return false;
		}
		rotation_user_t user;
		user.username = username;
		user.user_root = user_root;
		users.push_back(user);
	}
	closedir(directory);
	std::sort(users.begin(), users.end(),
	    [](const rotation_user_t &left, const rotation_user_t &right) {
		return left.username < right.username;
	});
	return true;
}

bool parse_nonnegative_number(const std::string &text, long long &value)
{
	if (text.empty())
		return false;
	char *end = NULL;
	errno = 0;
	const long long parsed = std::strtoll(text.c_str(), &end, 10);
	if (errno != 0 || end == NULL || *end != '\0' || parsed < 0)
		return false;
	value = parsed;
	return true;
}

// Test failures sometimes contain line breaks or very large upstream response
// fragments. Keep only a compact display-safe summary in the user's telemetry.
std::string sanitize_test_error(const std::string &input)
{
	std::string output;
	output.reserve(std::min<size_t>(input.size(), 512));
	for (size_t i = 0; i < input.size() && output.size() < 512; ++i) {
		const unsigned char c = static_cast<unsigned char>(input[i]);
		output.push_back(
		    c < 0x20 || c == 0x7f ? ' ' : static_cast<char>(c));
	}
	return output;
}

bool load_unlocked(const std::string &user_root,
    std::vector<provider_config_t> &providers, std::string &err)
{
	providers.clear();
	std::ifstream in(
	    provider_file(user_root).c_str(), std::ios::in | std::ios::binary);
	if (!in.good())
		return true;
	std::string line;
	if (!std::getline(in, line) || line != kHeader) {
		err = "invalid AI provider database";
		return false;
	}
	while (std::getline(in, line)) {
		if (line.empty())
			continue;
		std::vector<std::string> fields;
		split_tabs(line, fields);
		// The first ten columns are the original V1 record. Optional Responses
		// controls were appended so existing installations remain readable without
		// an eager migration or a second provider database.
		if (fields.size() != 10 && fields.size() != 19 &&
		    fields.size() != 21 && fields.size() != 22) {
			err = "invalid AI provider record";
			return false;
		}
		provider_config_t item;
		if (!hex_decode(fields[0], item.id) ||
		    !hex_decode(fields[1], item.name) ||
		    !hex_decode(fields[2], item.protocol) ||
		    !hex_decode(fields[3], item.base_url) ||
		    !hex_decode(fields[4], item.model) ||
		    !hex_decode(fields[5], item.api_key_ciphertext) ||
		    !hex_decode(fields[6], item.api_key_hint)) {
			err = "invalid AI provider encoding";
			return false;
		}
		item.enabled = fields[7] == "1";
		item.allow_file_content = fields[8] == "1";
		item.is_default = fields[9] == "1";
		if (fields.size() >= 19) {
			item.responses_store = fields[10] == "1";
			item.responses_background = fields[11] == "1";
			item.responses_compact = fields[12] == "1";
			item.responses_strict_tools = fields[13] == "1";
			if (!hex_decode(fields[14],
			        item.responses_min_reasoning_effort) ||
			    !hex_decode(
			        fields[15], item.responses_reasoning_summary) ||
			    !hex_decode(
			        fields[16], item.responses_text_verbosity) ||
			    !hex_decode(
			        fields[17], item.responses_service_tier) ||
			    !hex_decode(fields[18], item.responses_cache_ttl)) {
				err = "invalid AI provider Responses settings";
				return false;
			}
		}
		if (fields.size() >= 21 &&
		    (!hex_decode(fields[19], item.openai_organization) ||
		        !hex_decode(fields[20], item.openai_project))) {
			err = "invalid OpenAI routing header encoding";
			return false;
		}
		if (fields.size() == 22)
			item.qwen_session_cache = fields[21] == "1";
		item.output_limit_cache_directory = agent_dir(user_root);
		providers.push_back(item);
	}
	return true;
}

bool load_health_unlocked(const std::string &user_root,
    std::vector<provider_config_t> &providers, std::string &err)
{
	std::ifstream in(provider_health_file(user_root).c_str(),
	    std::ios::in | std::ios::binary);
	if (!in.good())
		return true;
	std::string line;
	if (!std::getline(in, line) || line != kHealthHeader) {
		err = "invalid AI provider health database";
		return false;
	}
	while (std::getline(in, line)) {
		if (line.empty())
			continue;
		std::vector<std::string> fields;
		split_tabs(line, fields);
		if (fields.size() != 6) {
			err = "invalid AI provider health record";
			return false;
		}
		std::string id;
		std::string status;
		std::string error_summary;
		long long tested_at = 0;
		long long http_status = 0;
		long long latency_ms = 0;
		if (!hex_decode(fields[0], id) ||
		    !hex_decode(fields[1], status) ||
		    !parse_nonnegative_number(fields[2], tested_at) ||
		    !parse_nonnegative_number(fields[3], http_status) ||
		    !parse_nonnegative_number(fields[4], latency_ms) ||
		    !hex_decode(fields[5], error_summary) || !valid_id(id) ||
		    (status != "ok" && status != "error") ||
		    http_status > 999 || error_summary.size() > 512) {
			err = "invalid AI provider health encoding";
			return false;
		}
		for (size_t i = 0; i < providers.size(); ++i) {
			if (providers[i].id != id)
				continue;
			providers[i].last_test_status = status;
			providers[i].last_test_at = tested_at;
			providers[i].last_test_http_status =
			    static_cast<int>(http_status);
			providers[i].last_test_latency_ms = latency_ms;
			providers[i].last_test_error = error_summary;
			break;
		}
	}
	return true;
}

bool save_health_unlocked(const std::string &user_root,
    const std::vector<provider_config_t> &providers, std::string &err)
{
	const std::string dir = agent_dir(user_root);
	if (!ensure_private_dir(dir, err))
		return false;
	const std::string path = provider_health_file(user_root);
	const std::string tmp = path + ".tmp";
	std::ofstream out(
	    tmp.c_str(), std::ios::out | std::ios::trunc | std::ios::binary);
	if (!out.good()) {
		err = "cannot write AI provider health database";
		return false;
	}
	out << kHealthHeader << '\n';
	for (size_t i = 0; i < providers.size(); ++i) {
		const provider_config_t &item = providers[i];
		if (item.last_test_status.empty() || item.last_test_at <= 0)
			continue;
		out << hex_encode(item.id) << '\t'
		    << hex_encode(item.last_test_status) << '\t'
		    << item.last_test_at << '\t' << item.last_test_http_status
		    << '\t' << item.last_test_latency_ms << '\t'
		    << hex_encode(item.last_test_error) << '\n';
	}
	out.close();
	if (!out.good()) {
		err = "cannot flush AI provider health database";
		return false;
	}
#ifndef _WIN32
	if (chmod(tmp.c_str(), 0600) != 0) {
		unlink(tmp.c_str());
		err = "cannot protect AI provider health database";
		return false;
	}
#endif
	if (!(rename(tmp.c_str(), path.c_str()) != 0))
		return true;
	err = std::string("cannot install AI provider health database: ") +
	    strerror(errno);
	return false;
}

bool save_unlocked(const std::string &user_root,
    const std::vector<provider_config_t> &providers, std::string &err)
{
	const std::string dir = agent_dir(user_root);
	if (!ensure_private_dir(dir, err))
		return false;
	const std::string path = provider_file(user_root);
	const std::string tmp = path + ".tmp";
	std::ofstream out(
	    tmp.c_str(), std::ios::out | std::ios::trunc | std::ios::binary);
	if (!out.good()) {
		err = "cannot write AI provider database";
		return false;
	}
	out << kHeader << '\n';
	for (size_t i = 0; i < providers.size(); ++i) {
		const provider_config_t &item = providers[i];
		out << hex_encode(item.id) << '\t' << hex_encode(item.name)
		    << '\t' << hex_encode(item.protocol) << '\t'
		    << hex_encode(item.base_url) << '\t'
		    << hex_encode(item.model) << '\t'
		    << hex_encode(item.api_key_ciphertext) << '\t'
		    << hex_encode(item.api_key_hint) << '\t'
		    << (item.enabled ? "1" : "0") << '\t'
		    << (item.allow_file_content ? "1" : "0") << '\t'
		    << (item.is_default ? "1" : "0") << '\t'
		    << (item.responses_store ? "1" : "0") << '\t'
		    << (item.responses_background ? "1" : "0") << '\t'
		    << (item.responses_compact ? "1" : "0") << '\t'
		    << (item.responses_strict_tools ? "1" : "0") << '\t'
		    << hex_encode(item.responses_min_reasoning_effort) << '\t'
		    << hex_encode(item.responses_reasoning_summary) << '\t'
		    << hex_encode(item.responses_text_verbosity) << '\t'
		    << hex_encode(item.responses_service_tier) << '\t'
		    << hex_encode(item.responses_cache_ttl) << '\t'
		    << hex_encode(item.openai_organization) << '\t'
		    << hex_encode(item.openai_project) << '\t'
		    << (item.qwen_session_cache ? "1" : "0") << '\n';
	}
	out.close();
	if (!out.good()) {
		err = "cannot flush AI provider database";
		return false;
	}
#ifndef _WIN32
	if (chmod(tmp.c_str(), 0600) != 0) {
		unlink(tmp.c_str());
		err = "cannot protect AI provider database";
		return false;
	}
#endif
	if (!(rename(tmp.c_str(), path.c_str()) != 0))
		return true;
	err = std::string("cannot install AI provider database: ") +
	    strerror(errno);
	return false;
}

}
}
}
