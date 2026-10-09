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

// Serializes change-set persistence, application and recovery.
extern webcool::mutex g_change_set_mutex;
// Record-format marker used to validate header v1.
extern const char *kHeaderV1;
// Record-format marker used to validate header v2.
extern const char *kHeaderV2;
// Record-format marker used to validate journal header v1.
extern const char *kJournalHeaderV1;
// Record-format marker used to validate journal header v2.
extern const char *kJournalHeaderV2;
// Maximum preview age in seconds before application is rejected.
extern const long long kLifetimeSeconds;
// Upper bound for items.
extern const size_t kMaxItems;
// Upper bound for item bytes.
extern const size_t kMaxItemBytes;
// Upper bound for total bytes.
extern const size_t kMaxTotalBytes;
// Upper bound for reason bytes.
extern const size_t kMaxReasonBytes;
// Upper bound for diff bytes.
extern const size_t kMaxDiffBytes;

// Validated change persisted for later application or rollback.
struct stored_change_t {
	// Requested change operation, such as write, delete or move.
	std::string operation;
	// Path of the file or resource associated with this record.
	std::string path;
	// Destination path for a move or related structural change.
	std::string target_path;
	// Text payload associated with this operation.
	std::string content;
	// Explanation supplied for this change or decision.
	std::string reason;
	// SHA-256 digest captured before applying the change.
	std::string original_sha256;
	// SHA-256 digest of the proposed replacement content.
	std::string proposed_sha256;
	// Whether applying the proposal creates a previously absent file.
	bool creates_file = false;
};

// Persisted change-set identity, creation time and ordered operations.
struct stored_set_t {
	// Identifier used to look up this record.
	std::string id;
	// Creation time as seconds since the Unix epoch.
	long long created_at = 0;
	// File proposals associated with this result or running task.
	std::vector<stored_change_t> changes;
};

using ::webcool::ai::file_ops::join_path;

using ::webcool::ai::record_codec::hex_value;

using ::webcool::ai::record_codec::hex_encode;

using ::webcool::ai::record_codec::hex_decode;

using ::webcool::ai::identifiers::valid_id;

using ::webcool::ai::identifiers::new_id;

using ::webcool::ai::file_ops::safe_directory;

// Ensure that the required storage directory exists.
bool ensure_directory(const std::string &path);

using ::webcool::ai::file_ops::replace_file;

// Resolve and validate the user's private change-set storage directory.
bool prepare_directory(
    const std::string &user_root, std::string &directory, std::string &err);

// Return the storage path used for pending.
std::string pending_path(const std::string &directory, const std::string &id);

// Return the storage path used for legacy pending.
std::string legacy_pending_path(const std::string &directory);

// Return the storage path used for journal.
std::string journal_path(const std::string &directory);

// Verify that a planned creation will not overwrite an existing entry.
bool target_absent(
    agent_workspace_t &workspace, const std::string &path, std::string &err);
// Undo an applied operation only while its expected state still matches.
bool rollback_change(agent_workspace_t &workspace,
    const stored_change_t &change, const std::string &original,
    std::string &err);

// Write recovery metadata before touching project files. fflush()+fsync() and
// an atomic replacement ensure a crash yields either the previous complete
// journal or the new complete journal, never a partially parsed record.
bool save_journal(const std::string &directory, const stored_set_t &set,
    const std::vector<std::string> &originals, std::string &err);

// Read the recovery journal needed to roll back an interrupted change set.
bool load_journal(const std::string &path, stored_set_t &set,
    std::vector<std::string> &originals, bool &found, std::string &err);

// Remove the recovery journal after the transaction is resolved.
bool remove_journal(const std::string &directory, std::string &err);

// Recover incomplete transactions while the caller holds the store mutex.
bool recover_locked(const std::string &directory, const std::string &user_root,
    std::string &err);

// Populate a bounded display diff for the proposed filesystem change.
void build_diff(const stored_change_t &change, const std::string &original,
    workspace_change_preview_item_t &item);

// Verify that a planned creation will not overwrite an existing entry.
bool target_absent(
    agent_workspace_t &workspace, const std::string &path, std::string &err);

// Construct the normalized directory-creation operation used by proposal
// validation.
bool mkdir_input(const workspace_change_input_t &change);

// Count path components for dependency ordering.
size_t path_depth(const std::string &path);

// Check whether earlier planned directory creation covers this path.
bool under_planned_directory(
    const std::string &path, const std::vector<std::string> &directories);

// Read source text and calculate the digest needed for transactional edits.
bool read_digest(agent_workspace_t &workspace, const std::string &path,
    std::string &digest, bool &absent, std::string &err);

// Undo an applied operation only while its expected state still matches.
bool rollback_change(agent_workspace_t &workspace,
    const stored_change_t &change, const std::string &original,
    std::string &err);

// Persist the validated change set for later confirmed application.
bool save_set(
    const std::string &directory, const stored_set_t &set, std::string &err);

// Read and validate the persisted change set with the expected identifier.
bool load_set(const std::string &path, const std::string &id, stored_set_t &set,
    std::string &err);

}
}
}
