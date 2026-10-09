#pragma once
#include <cstdio>
#include <string>
#ifdef _WIN32
#include "platform_compat.h"
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace webcool
{
namespace ai
{
namespace file_ops
{
// Lexical joining only: callers retain workspace containment/authorization checks.
inline std::string join_path(const std::string &left, const std::string &right)
{
	if (left.empty())
		return right;
	if (right.empty())
		return left;
	return left + "/" + right;
}
// Do not follow symlinks or Windows reparse points.
inline bool safe_directory(const std::string &path)
{
#ifdef _WIN32
	std::wstring wide;
	if (!webcool_utf8_path_to_wide(path.c_str(), wide))
		return false;
	const DWORD attributes = GetFileAttributesW(wide.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES &&
	       (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
	       (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
#else
	struct stat st;
	return lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode) &&
	       !S_ISLNK(st.st_mode);
#endif
}
// Single-level creation; existing permissions remain the caller's responsibility.
inline bool make_private_directory(const std::string &path)
{
	if (safe_directory(path))
		return true;
#ifdef _WIN32
	std::wstring wide;
	return webcool_utf8_path_to_wide(path.c_str(), wide) &&
	       _wmkdir(wide.c_str()) == 0 && safe_directory(path);
#else
	return mkdir(path.c_str(), 0700) == 0 && safe_directory(path);
#endif
}
inline bool path_entry_exists(const std::string &path)
{
#ifdef _WIN32
	std::wstring wide;
	return webcool_utf8_path_to_wide(path.c_str(), wide) &&
	       GetFileAttributesW(wide.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
	struct stat st;
	return lstat(path.c_str(), &st) == 0;
#endif
}
// Atomic replacement on the same filesystem. Durability/permissions and temp-file
// cleanup belong to each transaction; POSIX rename alone does not imply fsync.
inline bool replace_file(const std::string &temporary,
			 const std::string &target)
{
#ifdef _WIN32
	std::wstring temporary_wide, target_wide;
	return webcool_utf8_path_to_wide(temporary.c_str(), temporary_wide) &&
	       webcool_utf8_path_to_wide(target.c_str(), target_wide) &&
	       MoveFileExW(temporary_wide.c_str(), target_wide.c_str(),
			   MOVEFILE_REPLACE_EXISTING |
				   MOVEFILE_WRITE_THROUGH) != 0;
#else
	return rename(temporary.c_str(), target.c_str()) == 0;
#endif
}
}
}
}
