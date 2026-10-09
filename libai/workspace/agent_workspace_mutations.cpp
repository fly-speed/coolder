#include "stdafx.h"
#include "agent_workspace_internal.h"

namespace webcool
{
namespace ai
{

using namespace workspace_detail;

bool agent_workspace_t::replace_text_if_unchanged(
	const std::string &relative_file, const std::string &expected_sha256,
	const std::string &content, std::string &err) const
{
	if (content.size() > kMaxReadBytes || has_binary_bytes(content)) {
		err = "replacement must be a text file no larger than 1 MiB";
		return ai_error("agent.workspace", "validate-replacement", err);
	}
	std::string relative;
	if (!normalize_path(relative_file, relative, false, err))
		return false;
	std::string absolute;
	if (!resolve_existing(relative, absolute, err)) {
		return ai_error("agent.workspace", "resolve-replacement-file",
				err);
	}
	struct stat st;
	if (lstat(absolute.c_str(), &st) != 0 || is_link_like(absolute) ||
	    !S_ISREG(st.st_mode)) {
		err = "workspace path is not a regular file";
		return ai_error("agent.workspace", "validate-replacement-file",
				err);
	}
	std::string current;
	bool truncated = false;
	if (!read_limited(absolute, current, truncated, err) || truncated) {
		if (err.empty())
			err = "workspace file is too large to patch";
		return ai_error("agent.workspace", "read-replacement-original",
				err);
	}
	const std::string current_sha256 = content_sha256(current);
	if (current_sha256.empty()) {
		err = "cannot calculate workspace file digest";
		return ai_error("agent.workspace", "digest-original", err);
	}
	if (current_sha256 != expected_sha256) {
		err = "workspace file changed after patch preview";
		return ai_error("agent.workspace", "compare-original-digest",
				err);
	}
	const std::string id = random_hex_id();
	if (id.empty()) {
		err = "cannot create patch temporary file";
		return ai_error("agent.workspace", "generate-temporary-id",
				err);
	}
	const std::string temporary = absolute + ".webcool-patch-" + id;
	FILE *out = fopen(temporary.c_str(), "wb");
	if (out == NULL) {
		err = std::string("cannot create patch temporary file: ") +
		      strerror(errno);
		return ai_error("agent.workspace", "open-temporary-file", err);
	}
	const bool wrote = content.empty() ||
			   fwrite(content.data(), 1, content.size(), out) ==
				   content.size();
	bool flushed = wrote && fflush(out) == 0;
#ifdef _WIN32
	if (flushed)
		flushed = _commit(_fileno(out)) == 0;
#else
	if (flushed)
		flushed = fsync(fileno(out)) == 0;
#endif
	const bool closed = fclose(out) == 0;
	if (!flushed || !closed) {
		remove(temporary.c_str());
		err = "cannot flush patch temporary file";
		return ai_error("agent.workspace", "flush-temporary-file", err);
	}
#ifndef _WIN32
	if (chmod(temporary.c_str(), st.st_mode & 0777) != 0) {
		remove(temporary.c_str());
		err = "cannot preserve workspace file permissions";
		return ai_error("agent.workspace", "preserve-file-mode", err);
	}
#endif
	if (!replace_file(temporary, absolute)) {
		remove(temporary.c_str());
		err = std::string("cannot install workspace patch: ") +
		      strerror(errno);
		return ai_error("agent.workspace", "install-replacement", err);
	}
	return true;
}

bool agent_workspace_t::create_text_if_absent(const std::string &relative_file,
					      const std::string &content,
					      std::string &err) const
{
	if (content.size() > kMaxReadBytes || has_binary_bytes(content)) {
		err = "new file must be text no larger than 1 MiB";
		return ai_error("agent.workspace", "validate-new-file", err);
	}
	std::string relative;
	if (!normalize_path(relative_file, relative, false, err))
		return false;
	const size_t slash = relative.rfind('/');
	const std::string parent =
		slash == std::string::npos ? "" : relative.substr(0, slash);
	const std::string name = slash == std::string::npos ?
					 relative :
					 relative.substr(slash + 1);
	std::string parent_absolute;
	if (!resolve_existing(parent, parent_absolute, err)) {
		return ai_error("agent.workspace", "resolve-new-file-parent",
				err);
	}
	struct stat parent_st;
	if (stat(parent_absolute.c_str(), &parent_st) != 0 ||
	    !S_ISDIR(parent_st.st_mode)) {
		err = "new file parent is not a directory";
		return ai_error("agent.workspace", "validate-new-file-parent",
				err);
	}
	const std::string target = join_path(parent_absolute, name);
#ifdef _WIN32
	std::wstring target_wide;
	if (!webcool_utf8_path_to_wide(target.c_str(), target_wide)) {
		err = "cannot validate new workspace file path";
		return ai_error("agent.workspace", "encode-new-file-path", err);
	}
	if (GetFileAttributesW(target_wide.c_str()) !=
		    INVALID_FILE_ATTRIBUTES ||
	    GetLastError() != ERROR_FILE_NOT_FOUND)
#else
	struct stat target_st;
	if (lstat(target.c_str(), &target_st) == 0 || errno != ENOENT)
#endif
	{
		// This method never overwrites. The caller may propose a reviewed write
		// change when the target is an existing file.
		err = "workspace path already exists; use a reviewed write change to update it";
		return ai_error("agent.workspace", "check-new-file-absence",
				err);
	}
	std::string temporary;
	if (!write_temporary_text(target, content, temporary, err)) {
		return ai_error("agent.workspace", "write-new-file-temporary",
				err);
	}
#ifndef _WIN32
	if (chmod(temporary.c_str(), 0600) != 0) {
		remove(temporary.c_str());
		err = "cannot protect new workspace file";
		return ai_error("agent.workspace", "protect-new-file", err);
	}
#endif
	if (!install_new_file(temporary, target)) {
		remove(temporary.c_str());
		err = "cannot install new workspace file without overwriting";
		return ai_error("agent.workspace", "install-new-file", err);
	}
	return true;
}

bool agent_workspace_t::set_text_executable(const std::string &relative_file,
					    std::string &err) const
{
	std::string relative;
	if (!normalize_path(relative_file, relative, false, err))
		return false;
	std::string absolute;
	if (!resolve_existing(relative, absolute, err)) {
		return ai_error("agent.workspace", "resolve-executable-file",
				err);
	}
#ifdef _WIN32
	struct stat st;
	if (lstat(absolute.c_str(), &st) != 0 || is_link_like(absolute) ||
	    !S_ISREG(st.st_mode)) {
		err = "workspace executable path is not a regular file";
		return ai_error("agent.workspace", "validate-executable-file",
				err);
	}
#else
	const int fd = open(absolute.c_str(), O_RDONLY | O_NOFOLLOW);
	struct stat st;
	if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
		if (fd >= 0)
			close(fd);
		err = "workspace executable path is not a regular file";
		return ai_error("agent.workspace", "validate-executable-file",
				err);
	}
	if (fchmod(fd, 0755) != 0) {
		err = std::string("cannot make workspace file executable: ") +
		      strerror(errno);
		close(fd);
		return ai_error("agent.workspace", "chmod-executable-file",
				err);
	}
	if (close(fd) != 0) {
		err = "cannot close executable workspace file";
		return ai_error("agent.workspace", "close-executable-file",
				err);
	}
#endif
	return true;
}

bool agent_workspace_t::save_generated_text(const std::string &relative_file,
					    const std::string &content,
					    std::string &err) const
{
	if (content.size() > kMaxReadBytes || has_binary_bytes(content)) {
		err = "generated file must be text no larger than 1 MiB";
		return ai_error("agent.workspace", "validate-generated-file",
				err);
	}
	std::string relative;
	if (!normalize_path(relative_file, relative, false, err))
		return false;
	const size_t slash = relative.rfind('/');
	const std::string parent =
		slash == std::string::npos ? "" : relative.substr(0, slash);
	const std::string name = slash == std::string::npos ?
					 relative :
					 relative.substr(slash + 1);
	std::string parent_absolute;
	if (!resolve_existing(parent, parent_absolute, err)) {
		return ai_error("agent.workspace", "resolve-generated-parent",
				err);
	}
	const std::string target = join_path(parent_absolute, name);
	bool exists = false;
	int mode = 0600;
#ifdef _WIN32
	std::wstring target_wide;
	if (!webcool_utf8_path_to_wide(target.c_str(), target_wide)) {
		err = "cannot validate generated file path";
		return ai_error("agent.workspace", "encode-generated-path",
				err);
	}
	const DWORD attributes = GetFileAttributesW(target_wide.c_str());
	if (attributes != INVALID_FILE_ATTRIBUTES) {
		exists = true;
		if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
		    (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
			err = "generated file path is not a regular file";
			return ai_error("agent.workspace",
					"validate-generated-target", err);
		}
	} else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
		err = "cannot inspect generated file path";
		return ai_error("agent.workspace", "inspect-generated-target",
				err);
	}
#else
	struct stat st;
	if (lstat(target.c_str(), &st) == 0) {
		exists = true;
		if (!S_ISREG(st.st_mode) || is_link_like(target)) {
			err = "generated file path is not a regular file";
			return ai_error("agent.workspace",
					"validate-generated-target", err);
		}
		mode = st.st_mode & 0777;
	} else if (errno != ENOENT) {
		err = std::string("cannot inspect generated file path: ") +
		      strerror(errno);
		return ai_error("agent.workspace", "inspect-generated-target",
				err);
	}
#endif
	std::string temporary;
	if (!write_temporary_text(target, content, temporary, err)) {
		return ai_error("agent.workspace", "write-generated-temporary",
				err);
	}
#ifndef _WIN32
	if (chmod(temporary.c_str(), mode) != 0) {
		remove(temporary.c_str());
		err = "cannot protect generated text file";
		return ai_error("agent.workspace", "protect-generated-file",
				err);
	}
#endif
	const bool installed = exists ? replace_file(temporary, target) :
					install_new_file(temporary, target);
	if (!installed) {
		remove(temporary.c_str());
		err = std::string("cannot install generated text file: ") +
		      strerror(errno);
		return ai_error("agent.workspace", "install-generated-file",
				err);
	}
	return true;
}

bool agent_workspace_t::delete_text_if_unchanged(
	const std::string &relative_file, const std::string &expected_sha256,
	std::string &err) const
{
	std::string relative;
	if (!normalize_path(relative_file, relative, false, err))
		return false;
	std::string absolute;
	if (!resolve_existing(relative, absolute, err)) {
		return ai_error("agent.workspace", "resolve-delete-file", err);
	}
	struct stat st;
	if (lstat(absolute.c_str(), &st) != 0 || is_link_like(absolute) ||
	    !S_ISREG(st.st_mode)) {
		err = "workspace path is not a regular file";
		return ai_error("agent.workspace", "validate-delete-file", err);
	}
	std::string current;
	bool truncated = false;
	if (!read_limited(absolute, current, truncated, err) || truncated) {
		if (err.empty())
			err = "workspace file is too large to remove";
		return ai_error("agent.workspace", "read-delete-file", err);
	}
	if (content_sha256(current) != expected_sha256) {
		err = "workspace file changed before transactional rollback";
		return ai_error("agent.workspace", "compare-delete-digest",
				err);
	}
#ifdef _WIN32
	std::wstring absolute_wide;
	if (!webcool_utf8_path_to_wide(absolute.c_str(), absolute_wide) ||
	    DeleteFileW(absolute_wide.c_str()) == 0)
#else
	if (unlink(absolute.c_str()) != 0)
#endif
	{
		err = "cannot remove workspace file during transactional rollback";
		return ai_error("agent.workspace", "remove-rollback-file", err);
	}
	return true;
}

bool agent_workspace_t::create_directory_if_absent(
	const std::string &relative_directory, std::string &err) const
{
	std::string relative;
	if (!normalize_path(relative_directory, relative, false, err))
		return false;
	const size_t slash = relative.rfind('/');
	const std::string parent =
		slash == std::string::npos ? "" : relative.substr(0, slash);
	const std::string name = slash == std::string::npos ?
					 relative :
					 relative.substr(slash + 1);
	std::string parent_absolute;
	if (!resolve_existing(parent, parent_absolute, err)) {
		return ai_error("agent.workspace",
				"resolve-new-directory-parent", err);
	}
	const std::string target = join_path(parent_absolute, name);
#ifdef _WIN32
	std::wstring wide;
	if (!webcool_utf8_path_to_wide(target.c_str(), wide) ||
	    _wmkdir(wide.c_str()) != 0)
#else
	if (mkdir(target.c_str(), 0700) != 0)
#endif
	{
		err = errno == EEXIST ? "workspace path already exists" :
					"cannot create workspace directory";
		return ai_error("agent.workspace", "create-directory", err);
	}
	return true;
}

bool agent_workspace_t::delete_empty_directory(
	const std::string &relative_directory, std::string &err) const
{
	std::string relative;
	if (!normalize_path(relative_directory, relative, false, err))
		return false;
	std::string absolute;
	if (!resolve_existing(relative, absolute, err)) {
		return ai_error("agent.workspace", "resolve-empty-directory",
				err);
	}
	struct stat st;
	if (lstat(absolute.c_str(), &st) != 0 || is_link_like(absolute) ||
	    !S_ISDIR(st.st_mode)) {
		err = "workspace path is not a safe directory";
		return ai_error("agent.workspace", "validate-empty-directory",
				err);
	}
#ifdef _WIN32
	std::wstring wide;
	if (!webcool_utf8_path_to_wide(absolute.c_str(), wide) ||
	    _wrmdir(wide.c_str()) != 0)
#else
	if (rmdir(absolute.c_str()) != 0)
#endif
	{
		err = "workspace directory is not empty or cannot be removed";
		return ai_error("agent.workspace", "remove-empty-directory",
				err);
	}
	return true;
}

}
}
