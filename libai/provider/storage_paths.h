#pragma once
#include "../stdafx.h"
#include "../common/platform_compat.h"
#include <string>
namespace webcool { namespace ai { namespace storage_detail {
inline bool make_dir_recursive(const char* path) {
#ifdef _WIN32
 return webcool_make_dirs_utf8(path, 0755);
#else
 return acl_make_dirs(path, 0755) == 0;
#endif
}
// Provider metadata uses fixed private paths, never virtual shared-folder paths.
inline std::string join_path(const std::string& root, const std::string& path) {
 return path.empty() ? root : root + "/" + path;
}
}}}
