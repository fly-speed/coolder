#pragma once
#include "stdafx.h"
#include "../common/file_ops.h"
#include "../common/identifiers.h"
#include "../common/record_codec.h"
#include "agent_change_limits.h"
#include "workspace_change_set.h"
#include "agent_workspace.h"
#include "../common/ai_error_log.h"
#include "diff_preview.h"
#include "../common/webcool_mutex.h"

#ifdef _WIN32
#include "../common/platform_compat.h"
#include <direct.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <openssl/rand.h>

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
namespace change_set_detail
{

extern webcool::mutex g_change_set_mutex;
extern const char *kHeaderV1;
extern const char *kHeaderV2;
extern const char *kJournalHeaderV1;
extern const char *kJournalHeaderV2;
extern const long long kLifetimeSeconds;
extern const size_t kMaxItems;
extern const size_t kMaxItemBytes;
extern const size_t kMaxTotalBytes;
extern const size_t kMaxReasonBytes;
extern const size_t kMaxDiffBytes;

struct stored_change_t {
	std::string operation;
	std::string path;
	std::string target_path;
	std::string content;
	std::string reason;
	std::string original_sha256;
	std::string proposed_sha256;
	bool creates_file = false;
};

struct stored_set_t {
	std::string id;
	long long created_at = 0;
	std::vector<stored_change_t> changes;
};

using ::webcool::ai::file_ops::join_path;

using ::webcool::ai::record_codec::hex_value;

using ::webcool::ai::record_codec::hex_encode;

using ::webcool::ai::record_codec::hex_decode;

using ::webcool::ai::identifiers::valid_id;

using ::webcool::ai::identifiers::new_id;

using ::webcool::ai::file_ops::safe_directory;

bool ensure_directory(const std::string &path);

using ::webcool::ai::file_ops::replace_file;

bool prepare_directory(
    const std::string &user_root, std::string &directory, std::string &err);

std::string pending_path(const std::string &directory, const std::string &id);

std::string legacy_pending_path(const std::string &directory);

std::string journal_path(const std::string &directory);

bool target_absent(
    agent_workspace_t &workspace, const std::string &path, std::string &err);
bool rollback_change(agent_workspace_t &workspace,
    const stored_change_t &change, const std::string &original,
    std::string &err);

// Write recovery metadata before touching project files. fflush()+fsync() and
// an atomic replacement ensure a crash yields either the previous complete
// journal or the new complete journal, never a partially parsed record.
bool save_journal(const std::string &directory, const stored_set_t &set,
    const std::vector<std::string> &originals, std::string &err);

bool load_journal(const std::string &path, stored_set_t &set,
    std::vector<std::string> &originals, bool &found, std::string &err);

bool remove_journal(const std::string &directory, std::string &err);

bool recover_locked(const std::string &directory, const std::string &user_root,
    std::string &err);

void build_diff(const stored_change_t &change, const std::string &original,
    workspace_change_preview_item_t &item);

bool target_absent(
    agent_workspace_t &workspace, const std::string &path, std::string &err);

bool mkdir_input(const workspace_change_input_t &change);

size_t path_depth(const std::string &path);

bool under_planned_directory(
    const std::string &path, const std::vector<std::string> &directories);

bool read_digest(agent_workspace_t &workspace, const std::string &path,
    std::string &digest, bool &absent, std::string &err);

bool rollback_change(agent_workspace_t &workspace,
    const stored_change_t &change, const std::string &original,
    std::string &err);

bool save_set(
    const std::string &directory, const stored_set_t &set, std::string &err);

bool load_set(const std::string &path, const std::string &id, stored_set_t &set,
    std::string &err);

}
}
}
