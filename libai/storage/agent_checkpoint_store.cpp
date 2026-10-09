#include "stdafx.h"
#include "../common/file_ops.h"
#include "../common/identifiers.h"
#include "../common/record_codec.h"
#include "agent_checkpoint_store.h"
#include "../common/ai_error_log.h"
#include "../provider/ai_provider_store.h"
#include "../common/webcool_mutex.h"

#ifdef _WIN32
#include "../common/platform_compat.h"
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <vector>

namespace webcool
{
namespace ai
{
namespace
{

webcool::mutex g_checkpoint_mutex;
const char *kHeader = "WEBCOOL_AGENT_CHECKPOINT_V1";

using ::webcool::ai::file_ops::join_path;

using ::webcool::ai::record_codec::hex_value;

using ::webcool::ai::identifiers::valid_id;

using ::webcool::ai::record_codec::hex_encode;

using ::webcool::ai::record_codec::hex_decode;

using ::webcool::ai::record_codec::split_tabs;

bool safe_checkpoint(const agent_checkpoint_t &checkpoint)
{
	return valid_id(checkpoint.run_id) && !checkpoint.provider_id.empty() &&
	    checkpoint.provider_id.size() <= 64 &&
	    checkpoint.project_path.size() <= 2048 &&
	    (checkpoint.response_state_mode == "auto" ||
	        checkpoint.response_state_mode == "stateless" ||
	        checkpoint.response_state_mode == "stateful") &&
	    (checkpoint.ui_language == "zh" ||
	        checkpoint.ui_language == "en") &&
	    !checkpoint.prompt.empty() &&
	    checkpoint.prompt.size() <= 32 * 1024 &&
	    (checkpoint.session_id.empty() ||
	        valid_id(checkpoint.session_id)) &&
	    checkpoint.max_output_tokens >= 64 &&
	    checkpoint.max_output_tokens <= 1000000 &&
	    (checkpoint.thinking_mode.empty() ||
	        checkpoint.thinking_mode == "enabled" ||
	        checkpoint.thinking_mode == "disabled") &&
	    (checkpoint.reasoning_effort.empty() ||
	        checkpoint.reasoning_effort == "low" ||
	        checkpoint.reasoning_effort == "high" ||
	        checkpoint.reasoning_effort == "max") &&
	    (checkpoint.execution_mode.empty() ||
	        checkpoint.execution_mode == "quick" ||
	        checkpoint.execution_mode == "standard" ||
	        checkpoint.execution_mode == "large");
}

std::string serialize(const agent_checkpoint_t &checkpoint)
{
	return hex_encode(checkpoint.run_id) + "\t" +
	    hex_encode(checkpoint.provider_id) + "\t" +
	    hex_encode(checkpoint.project_path) + "\t" +
	    hex_encode(checkpoint.prompt) + "\t" +
	    hex_encode(checkpoint.session_id) + "\t" +
	    std::to_string(checkpoint.max_output_tokens) + "\t" +
	    (checkpoint.remember_session ? "1" : "0") + "\t" +
	    hex_encode(checkpoint.thinking_mode) + "\t" +
	    hex_encode(checkpoint.execution_mode) + "\t" +
	    hex_encode(checkpoint.reasoning_effort) + "\t" +
	    hex_encode(checkpoint.ui_language) + "\t" +
	    checkpoint.response_state_mode;
}

bool deserialize(const std::string &value, agent_checkpoint_t &checkpoint)
{
	std::vector<std::string> fields;
	split_tabs(value, fields);
	// Seven fields are the original V1 layout. Accepting it keeps unfinished
	// runs recoverable after upgrading a server in place.
	if (fields.size() < 7 || fields.size() > 12) {
		return false;
	}
	char *end = NULL;
	errno = 0;
	const long long max_tokens = strtoll(fields[5].c_str(), &end, 10);
	if (!hex_decode(fields[0], checkpoint.run_id) ||
	    !hex_decode(fields[1], checkpoint.provider_id) ||
	    !hex_decode(fields[2], checkpoint.project_path) ||
	    !hex_decode(fields[3], checkpoint.prompt) ||
	    !hex_decode(fields[4], checkpoint.session_id) || errno != 0 ||
	    end == fields[5].c_str() || *end != '\0' ||
	    (fields[6] != "0" && fields[6] != "1")) {
		return false;
	}
	checkpoint.max_output_tokens = max_tokens;
	checkpoint.remember_session = fields[6] == "1";
	checkpoint.thinking_mode.clear();
	checkpoint.reasoning_effort.clear();
	checkpoint.execution_mode = "standard";
	if (fields.size() >= 8 &&
	    !hex_decode(fields[7], checkpoint.thinking_mode))
		return false;
	if (fields.size() >= 9 &&
	    (!hex_decode(fields[7], checkpoint.thinking_mode) ||
	        !hex_decode(fields[8], checkpoint.execution_mode)))
		return false;
	if (fields.size() >= 10 &&
	    !hex_decode(fields[9], checkpoint.reasoning_effort))
		return false;
	checkpoint.ui_language = "zh";
	if (fields.size() >= 11 &&
	    !hex_decode(fields[10], checkpoint.ui_language))
		return false;
	checkpoint.response_state_mode = "auto";
	if (!(fields.size() >= 12))
		return safe_checkpoint(checkpoint);
	// Accept checkpoints written by the previous checkbox version.
	checkpoint.response_state_mode = fields[11] == "1" ? "stateless" :
	    fields[11] == "0"                              ? "auto" :
	                                                     fields[11];

	return safe_checkpoint(checkpoint);
}

std::string checkpoint_dir(const std::string &user_root)
{
	return join_path(user_root, ".webcool_agent/checkpoints");
}

std::string checkpoint_path(
    const std::string &user_root, const std::string &run_id)
{
	return join_path(checkpoint_dir(user_root), run_id + ".v1");
}

bool ensure_directory(const std::string &user_root, std::string &err)
{
	const std::string agent = join_path(user_root, ".webcool_agent");
	const std::string directory = checkpoint_dir(user_root);
#ifdef _WIN32
	std::wstring agent_wide;
	std::wstring directory_wide;
	if (!webcool_utf8_path_to_wide(agent.c_str(), agent_wide) ||
	    !webcool_utf8_path_to_wide(directory.c_str(), directory_wide)) {
		err = "cannot validate agent checkpoint directory";
		return false;
	}
	if (GetFileAttributesW(agent_wide.c_str()) == INVALID_FILE_ATTRIBUTES &&
	    _wmkdir(agent_wide.c_str()) != 0) {
		err = "cannot create agent checkpoint parent directory";
		return false;
	}
	if (GetFileAttributesW(directory_wide.c_str()) ==
	        INVALID_FILE_ATTRIBUTES &&
	    _wmkdir(directory_wide.c_str()) != 0) {
		err = "cannot create agent checkpoint directory";
		return false;
	}
	const DWORD attributes = GetFileAttributesW(directory_wide.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES ||
	    (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
	    (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
		err = "agent checkpoint path is not a safe directory";
		return false;
	}
#else
	struct stat st;
	if (lstat(agent.c_str(), &st) != 0 && mkdir(agent.c_str(), 0700) != 0) {
		err = "cannot create agent checkpoint parent directory";
		return false;
	}
	if (lstat(directory.c_str(), &st) != 0 &&
	    mkdir(directory.c_str(), 0700) != 0) {
		err = "cannot create agent checkpoint directory";
		return false;
	}
	if (lstat(directory.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) ||
	    S_ISLNK(st.st_mode) || chmod(directory.c_str(), 0700) != 0) {
		err = "agent checkpoint path is not a safe private directory";
		return false;
	}
#endif
	return true;
}

using ::webcool::ai::file_ops::replace_file;

} // namespace

agent_checkpoint_store_t::agent_checkpoint_store_t(
    const std::string &upload_root, const std::string &user_root,
    const std::string &username)
        : upload_root_(upload_root)
        , user_root_(user_root)
        , username_(username)
{
}

bool agent_checkpoint_store_t::save(
    const agent_checkpoint_t &checkpoint, std::string &err) const
{
	if (!safe_checkpoint(checkpoint)) {
		err = "invalid agent restart checkpoint";
		return ai_error("agent.checkpoint", "validate-save", err);
	}
	std::string ciphertext;
	if (!provider_store_t::seal_user_data(upload_root_, username_,
	        "agent-checkpoint:" + checkpoint.run_id, serialize(checkpoint),
	        ciphertext, err)) {
		return ai_error("agent.checkpoint", "encrypt", err);
	}
	std::lock_guard<webcool::mutex> guard(g_checkpoint_mutex);
	if (!ensure_directory(user_root_, err)) {
		return ai_error("agent.checkpoint", "prepare-directory", err);
	}
	const std::string path = checkpoint_path(user_root_, checkpoint.run_id);
	const std::string temporary = path + ".tmp";
	std::ofstream out(temporary.c_str(),
	    std::ios::out | std::ios::binary | std::ios::trunc);
	if (!out.good()) {
		err = "cannot write agent restart checkpoint";
		return ai_error("agent.checkpoint", "write", err);
	}
	out << kHeader << '\n' << ciphertext << '\n';
	out.close();
	if (!out.good()) {
		err = "cannot flush agent restart checkpoint";
		return ai_error("agent.checkpoint", "flush", err);
	}
#ifndef _WIN32
	if (chmod(temporary.c_str(), 0600) != 0) {
		unlink(temporary.c_str());
		err = "cannot protect agent restart checkpoint";
		return ai_error("agent.checkpoint", "protect", err);
	}
#endif
	if (replace_file(temporary, path))
		return true;
	::remove(temporary.c_str());
	err = std::string("cannot install agent restart checkpoint: ") +
	    strerror(errno);
	return ai_error("agent.checkpoint", "install", err);
}

bool agent_checkpoint_store_t::load(const std::string &run_id,
    agent_checkpoint_t &checkpoint, std::string &err) const
{
	if (!valid_id(run_id)) {
		err = "invalid agent run id";
		return ai_error("agent.checkpoint", "validate-load", err);
	}
	std::string ciphertext;
	{
		std::lock_guard<webcool::mutex> guard(g_checkpoint_mutex);
		std::ifstream in(checkpoint_path(user_root_, run_id).c_str(),
		    std::ios::in | std::ios::binary);
		if (!in.good()) {
			err = "agent restart checkpoint not found";
			return ai_error("agent.checkpoint", "find", err);
		}
		std::string header;
		std::string extra;
		if (!std::getline(in, header) || header != kHeader ||
		    !std::getline(in, ciphertext) || std::getline(in, extra)) {
			err = "invalid agent restart checkpoint file";
			return ai_error("agent.checkpoint", "parse-file", err);
		}
	}
	std::string plaintext;
	if (!provider_store_t::open_user_data(upload_root_, username_,
	        "agent-checkpoint:" + run_id, ciphertext, plaintext, err)) {
		return ai_error("agent.checkpoint", "decrypt", err);
	}
	const bool parsed =
	    deserialize(plaintext, checkpoint) && checkpoint.run_id == run_id;
	std::fill(plaintext.begin(), plaintext.end(), '\0');
	if (parsed)
		return true;
	err = "invalid agent restart checkpoint payload";
	return ai_error("agent.checkpoint", "parse-payload", err);
}

bool agent_checkpoint_store_t::remove(
    const std::string &run_id, std::string &err) const
{
	if (!valid_id(run_id)) {
		err = "invalid agent run id";
		return ai_error("agent.checkpoint", "validate-remove", err);
	}
	std::lock_guard<webcool::mutex> guard(g_checkpoint_mutex);
	const std::string path = checkpoint_path(user_root_, run_id);
	if (!(::remove(path.c_str()) != 0 && errno != ENOENT))
		return true;
	err = std::string("cannot remove agent restart checkpoint: ") +
	    strerror(errno);
	return ai_error("agent.checkpoint", "remove", err);
}

bool agent_checkpoint_store_t::exists(const std::string &run_id) const
{
	if (!valid_id(run_id))
		return false;
	std::lock_guard<webcool::mutex> guard(g_checkpoint_mutex);
	struct stat st;
	return stat(checkpoint_path(user_root_, run_id).c_str(), &st) == 0 &&
	    S_ISREG(st.st_mode);
}

} // namespace ai
} // namespace webcool
