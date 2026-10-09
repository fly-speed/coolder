#include "stdafx.h"
#include "../common/file_ops.h"
#include "../common/identifiers.h"
#include "../common/record_codec.h"
#include "workspace_patch.h"
#include "agent_workspace.h"
#include "../common/ai_error_log.h"
#include "diff_preview.h"
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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <vector>

namespace webcool
{
namespace ai
{
namespace
{

// A pending plan contains the proposed content because confirmation may arrive
// in a later request. It is private to the current user, bounded to 1 MiB and
// removed after successful application or expiry.
webcool::mutex g_patch_store_mutex;
const char *kPatchHeader = "WEBCOOL_WORKSPACE_PATCH_V1";
const long long kPatchLifetimeSeconds = 30 * 60;
const size_t kMaxPatchBytes = 1024 * 1024;
const size_t kMaxPreviewLinesPerSide = 120;
const size_t kMaxPreviewBytes = 128 * 1024;

struct stored_patch_t {
	// original_sha256 provides compare-and-swap semantics; proposed_sha256
	// validates the private plan file before it can be applied.
	std::string id;
	std::string path;
	std::string original_sha256;
	std::string proposed_sha256;
	std::string content;
	long long created_at;
};

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

bool replace_plan_file(const std::string &temporary, const std::string &target)
{
	return ::webcool::ai::file_ops::replace_file(temporary, target);
}

bool ensure_plan_directory(const std::string &user_root, std::string &directory,
			   std::string &err)
{
	const std::string agent = join_path(user_root, ".webcool_agent");
	directory = join_path(agent, "patches");
	if (!ensure_directory(agent) || !ensure_directory(directory)) {
		err = "cannot create a safe workspace patch directory";
		return false;
	}
#ifndef _WIN32
	if (chmod(agent.c_str(), 0700) != 0 ||
	    chmod(directory.c_str(), 0700) != 0) {
		err = "cannot protect workspace patch directory";
		return false;
	}
#endif
	return true;
}

std::string plan_path(const std::string &directory)
{
	return join_path(directory, "pending.v1");
}

void build_preview(const std::string &path, const std::string &original,
		   const std::string &proposed,
		   workspace_patch_preview_t &preview)
{
	const line_diff_preview_t diff(original, proposed);
	const std::vector<line_diff_op_t> &operations = diff.operations;
	preview.removed_lines = diff.removed_lines;
	preview.added_lines = diff.added_lines;
	std::ostringstream out;
	out << "--- " << path << '\n' << "+++ " << path << '\n';
	const std::vector<bool> &visible = diff.visible;
	bool gap = false;
	size_t removed_shown = 0;
	size_t added_shown = 0;
	for (size_t i = 0; i < operations.size(); ++i) {
		if (!visible[i]) {
			gap = true;
			continue;
		}
		if (gap) {
			out << "@@ unchanged lines omitted @@\n";
			gap = false;
		}
		const line_diff_op_t &operation = operations[i];
		if (operation.kind == line_diff_removed &&
		    removed_shown++ >= kMaxPreviewLinesPerSide)
			continue;
		if (operation.kind == line_diff_added &&
		    added_shown++ >= kMaxPreviewLinesPerSide)
			continue;
		diff.append_line(out, operation);
	}
	if (removed_shown > kMaxPreviewLinesPerSide)
		out << "- ... removed lines truncated ...\n";
	if (added_shown > kMaxPreviewLinesPerSide)
		out << "+ ... added lines truncated ...\n";
	preview.diff = out.str();
	if (preview.diff.size() > kMaxPreviewBytes) {
		preview.diff.resize(kMaxPreviewBytes);
		preview.diff += "\n... preview truncated ...\n";
	}
}

bool save_patch(const std::string &directory, const stored_patch_t &patch,
		std::string &err)
{
	const std::string path = plan_path(directory);
	const std::string temporary = path + ".tmp";
	std::ofstream out(temporary.c_str(),
			  std::ios::out | std::ios::binary | std::ios::trunc);
	if (!out.good()) {
		err = "cannot write workspace patch plan";
		return false;
	}
	out << kPatchHeader << '\n'
	    << patch.created_at << '\n'
	    << patch.id << '\n'
	    << hex_encode(patch.path) << '\n'
	    << patch.original_sha256 << '\n'
	    << patch.proposed_sha256 << '\n'
	    << hex_encode(patch.content) << '\n';
	out.close();
	if (!out.good()) {
		remove(temporary.c_str());
		err = "cannot flush workspace patch plan";
		return false;
	}
#ifndef _WIN32
	if (chmod(temporary.c_str(), 0600) != 0) {
		remove(temporary.c_str());
		err = "cannot protect workspace patch plan";
		return false;
	}
#endif
	if (!replace_plan_file(temporary, path)) {
		remove(temporary.c_str());
		err = std::string("cannot install workspace patch plan: ") +
		      strerror(errno);
		return false;
	}
	return true;
}

bool load_patch(const std::string &directory, const std::string &id,
		stored_patch_t &patch, std::string &err)
{
	if (!valid_id(id)) {
		err = "invalid workspace patch id";
		return false;
	}
	const std::string path = plan_path(directory);
	std::ifstream in(path.c_str(), std::ios::in | std::ios::binary);
	if (!in.good()) {
		err = "workspace patch plan not found";
		return false;
	}
	std::string header;
	std::string created;
	std::string stored_id;
	std::string encoded_path;
	std::string encoded_content;
	if (!std::getline(in, header) || header != kPatchHeader ||
	    !std::getline(in, created) || !std::getline(in, stored_id) ||
	    stored_id != id || !std::getline(in, encoded_path) ||
	    !std::getline(in, patch.original_sha256) ||
	    !std::getline(in, patch.proposed_sha256) ||
	    !std::getline(in, encoded_content) ||
	    !hex_decode(encoded_path, patch.path) ||
	    !hex_decode(encoded_content, patch.content)) {
		err = "invalid workspace patch plan";
		return false;
	}
	char *end = NULL;
	patch.created_at = strtoll(created.c_str(), &end, 10);
	if (end == created.c_str() || *end != '\0') {
		err = "invalid workspace patch timestamp";
		return false;
	}
	patch.id = stored_id;
	if (patch.content.size() > kMaxPatchBytes ||
	    agent_workspace_t::content_sha256(patch.content) !=
		    patch.proposed_sha256) {
		err = "workspace patch plan failed integrity validation";
		return false;
	}
	return true;
}

} // namespace

workspace_patch_store_t::workspace_patch_store_t(const std::string &user_root)
	: user_root_(user_root)
{
}

bool workspace_patch_store_t::create(const std::string &relative_file,
				     const std::string &proposed_content,
				     workspace_patch_preview_t &preview,
				     std::string &err) const
{
	if (proposed_content.size() > kMaxPatchBytes ||
	    proposed_content.find('\0') != std::string::npos) {
		err = "proposed file must be text no larger than 1 MiB";
		return ai_error("workspace.patch", "validate-proposal", err);
	}
	agent_workspace_t workspace(user_root_);
	std::string normalized;
	if (!agent_workspace_t::normalize_path(relative_file, normalized, false,
					       err)) {
		return ai_error("workspace.patch", "normalize-path", err);
	}
	std::string original;
	bool truncated = false;
	if (!workspace.read(normalized, original, truncated, err)) {
		return ai_error("workspace.patch", "read-original", err);
	}
	if (truncated) {
		err = "workspace file is too large to patch";
		return ai_error("workspace.patch", "validate-original-size",
				err);
	}
	if (original == proposed_content) {
		err = "proposed file is identical to the current file";
		return ai_error("workspace.patch", "compare-content", err);
	}
	stored_patch_t patch;
	patch.id = new_id();
	patch.path = normalized;
	patch.original_sha256 = agent_workspace_t::content_sha256(original);
	patch.proposed_sha256 =
		agent_workspace_t::content_sha256(proposed_content);
	patch.content = proposed_content;
	patch.created_at = static_cast<long long>(time(NULL));
	if (patch.id.empty() || patch.original_sha256.empty() ||
	    patch.proposed_sha256.empty()) {
		err = "cannot initialize workspace patch plan";
		return ai_error("workspace.patch", "initialize-plan", err);
	}
	preview.id = patch.id;
	preview.path = patch.path;
	preview.original_sha256 = patch.original_sha256;
	preview.proposed_sha256 = patch.proposed_sha256;
	build_preview(patch.path, original, proposed_content, preview);
	std::lock_guard<webcool::mutex> guard(g_patch_store_mutex);
	std::string directory;
	if (!ensure_plan_directory(user_root_, directory, err)) {
		return ai_error("workspace.patch", "prepare-private-store",
				err);
	}
	if (!save_patch(directory, patch, err)) {
		return ai_error("workspace.patch", "save-plan", err);
	}
	return true;
}

bool workspace_patch_store_t::apply(const std::string &patch_id,
				    workspace_patch_result_t &result,
				    std::string &err) const
{
	std::lock_guard<webcool::mutex> guard(g_patch_store_mutex);
	std::string directory;
	if (!ensure_plan_directory(user_root_, directory, err)) {
		return ai_error("workspace.patch", "prepare-apply-store", err);
	}
	stored_patch_t patch;
	if (!load_patch(directory, patch_id, patch, err)) {
		return ai_error("workspace.patch", "load-plan", err);
	}
	const long long now = static_cast<long long>(time(NULL));
	if (patch.created_at <= 0 || now < patch.created_at ||
	    now - patch.created_at > kPatchLifetimeSeconds) {
		remove(plan_path(directory).c_str());
		err = "workspace patch plan expired";
		return ai_error("workspace.patch", "check-expiry", err);
	}
	agent_workspace_t workspace(user_root_);
	if (!workspace.replace_text_if_unchanged(
		    patch.path, patch.original_sha256, patch.content, err)) {
		return ai_error("workspace.patch", "compare-and-replace", err);
	}
	remove(plan_path(directory).c_str());
	result.id = patch.id;
	result.path = patch.path;
	result.sha256 = patch.proposed_sha256;
	return true;
}

} // namespace ai
} // namespace webcool
