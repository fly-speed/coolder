#include "stdafx.h"
#include "agent_workspace_internal.h"

namespace webcool
{
namespace ai
{

namespace workspace_detail
{

std::string ascii_lower(const std::string &value)
{
	std::string lower = value;
	for (size_t i = 0; i < lower.size(); ++i) {
		if (!(lower[i] >= 'A' && lower[i] <= 'Z'))
			continue;
		lower[i] += 'a' - 'A';
	}
	return lower;
}

bool has_path_component(const std::string &path, const std::string &wanted)
{
	size_t begin = 0;
	while (begin <= path.size()) {
		const size_t end = path.find('/', begin);
		if (path.substr(begin,
		        end == std::string::npos ? std::string::npos :
		                                   end - begin) == wanted)
			return true;
		if (end == std::string::npos)
			break;
		begin = end + 1;
	}
	return false;
}

bool path_has_prefix(const std::string &path, const std::string &prefix)
{
	return path == prefix ||
	    (path.size() > prefix.size() &&
	        path.compare(0, prefix.size(), prefix) == 0 &&
	        path[prefix.size()] == '/');
}

bool has_suffix(const std::string &value, const char *suffix)
{
	const size_t length = strlen(suffix);
	return value.size() >= length &&
	    value.compare(value.size() - length, length, suffix) == 0;
}

using ::webcool::ai::file_ops::join_path;

bool path_is_within(const std::string &root, const std::string &candidate)
{
	if (candidate == root)
		return true;
	if (candidate.size() <= root.size())
		return false;
	if (candidate.compare(0, root.size(), root) != 0)
		return false;
	const char boundary = candidate[root.size()];
	return boundary == '/' || boundary == '\\';
}

// user may type the name of a project directory that has not been created yet;
// reporting that case as a symbolic link hides the real configuration error.
path_component_state_t inspect_path_component(const std::string &path)
{
#ifdef _WIN32
	std::wstring wide;
	if (!webcool_utf8_path_to_wide(path.c_str(), wide)) {
		return PATH_COMPONENT_ERROR;
	}
	const DWORD attrs = GetFileAttributesW(wide.c_str());
	if (attrs == INVALID_FILE_ATTRIBUTES) {
		const DWORD error = GetLastError();
		return error == ERROR_FILE_NOT_FOUND ||
		        error == ERROR_PATH_NOT_FOUND ?
		    PATH_COMPONENT_MISSING :
		    PATH_COMPONENT_ERROR;
	}
	return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ?
	    PATH_COMPONENT_LINK :
	    PATH_COMPONENT_NORMAL;
#else
	struct stat st;
	if (lstat(path.c_str(), &st) != 0) {
		return errno == ENOENT || errno == ENOTDIR ?
		    PATH_COMPONENT_MISSING :
		    PATH_COMPONENT_ERROR;
	}
	return S_ISLNK(st.st_mode) ? PATH_COMPONENT_LINK :
	                             PATH_COMPONENT_NORMAL;
#endif
}

// Directory traversal and mutation helpers operate only on paths that must
// already exist. Preserve their fail-closed behavior for missing/unreadable
// entries, while resolve_existing() below reports the precise state to users.
bool is_link_like(const std::string &path)
{
	return inspect_path_component(path) != PATH_COMPONENT_NORMAL;
}

path_component_state_t inspect_path_components(
    const std::string &root, const std::string &relative)
{
	// Check every component before realpath(). Without this pass, an existing
	// symlink that resolves back inside the root could still bypass the explicit
	// "no links" workspace policy.
	std::string current = root;
	size_t begin = 0;
	while (begin < relative.size()) {
		const size_t end = relative.find('/', begin);
		const std::string part = relative.substr(begin,
		    end == std::string::npos ? std::string::npos : end - begin);
		if (!part.empty()) {
			current = join_path(current, part);
			const path_component_state_t state =
			    inspect_path_component(current);
			if (state != PATH_COMPONENT_NORMAL)
				return state;
		}
		if (end == std::string::npos)
			break;
		begin = end + 1;
	}
	return PATH_COMPONENT_NORMAL;
}

} // namespace workspace_detail
using namespace workspace_detail;

agent_workspace_t::agent_workspace_t(
    const std::string &user_root, bool allow_external_roots)
        : user_root_(user_root)
        , allow_external_roots_(allow_external_roots)
{
}

bool agent_workspace_t::normalize_path(const std::string &input,
    std::string &normalized, bool allow_empty, std::string &err)
{
	normalized.clear();
	std::string segment;
	for (size_t i = 0; i <= input.size(); ++i) {
		const char ch = i < input.size() ? input[i] : '/';
		if (ch == '\0') {
			err = "workspace path contains a null byte";
			return ai_error(
			    "agent.workspace", "normalize-null-byte", err);
		}
		if (ch == '/' || ch == '\\') {
			if (segment.empty() || segment == ".") {
				segment.clear();
				continue;
			}
			if (segment == "..") {
				err =
				    "workspace path cannot leave the user directory";
				return ai_error("agent.workspace",
				    "normalize-parent-segment", err);
			}
			if (!normalized.empty())
				normalized += "/";
			normalized += segment;
			segment.clear();
		} else {
			if (static_cast<unsigned char>(ch) < 32) {
				err =
				    "workspace path contains control characters";
				return ai_error("agent.workspace",
				    "normalize-control-character", err);
			}
			segment += ch;
		}
	}
	if (!allow_empty && normalized.empty()) {
		err = "workspace path is required";
		return ai_error(
		    "agent.workspace", "normalize-required-path", err);
	}
	if (!(!normalized.empty() && path_is_sensitive(normalized)))
		return true;
	err = "workspace path is protected by the sensitive-file policy";
	return ai_error("agent.workspace", "normalize-sensitive-path", err);
}

bool agent_workspace_t::path_is_sensitive(const std::string &normalized_path)
{
	const std::string path = ascii_lower(normalized_path);
	if (path.empty())
		return false;
	if (ai_extra_sensitive_path(path))
		return true;

	// These entries commonly contain repository credentials, deployment secrets
	// or data that should not be copied into a third-party model request.
	if (has_path_component(path, ".mailbox") ||
	    has_path_component(path, ".webcool_agent") ||
	    has_path_component(path, ".git") ||
	    has_path_component(path, ".hg") || has_path_component(path, ".svn"))
		return true;

	const size_t slash = path.rfind('/');
	const std::string name =
	    slash == std::string::npos ? path : path.substr(slash + 1);
	if (name == ".env" || name.compare(0, 5, ".env.") == 0 ||
	    name == "id_rsa" || name == "id_ed25519" || name == "credentials" ||
	    name == "credentials.json" || name == "service-account.json" ||
	    has_suffix(name, ".pem") || has_suffix(name, ".key") ||
	    has_suffix(name, ".p12") || has_suffix(name, ".pfx") ||
	    has_suffix(name, ".db") || has_suffix(name, ".sqlite") ||
	    has_suffix(name, ".sqlite3")) {
		return true;
	}

	// Extra entries are relative path prefixes, not shell patterns. Ignoring
	// empty/absolute/parent entries keeps a malformed environment setting from
	// unexpectedly widening access or changing the meaning of the workspace.
	const char *configured = getenv("WEBCOOL_AI_SENSITIVE_PATHS");
	if (configured == NULL)
		return false;
	const std::string value(configured);
	size_t begin = 0;
	while (begin <= value.size()) {
		const size_t end = value.find(',', begin);
		std::string prefix = ascii_lower(value.substr(begin,
		    end == std::string::npos ? std::string::npos :
		                               end - begin));
		while (!prefix.empty() &&
		    (prefix[0] == ' ' || prefix[0] == '\t')) {
			prefix.erase(0, 1);
		}
		while (!prefix.empty() &&
		    (prefix[prefix.size() - 1] == ' ' ||
		        prefix[prefix.size() - 1] == '\t'))
			prefix.resize(prefix.size() - 1);
		if (!prefix.empty() && prefix[0] != '/' &&
		    prefix.find("..") == std::string::npos &&
		    path_has_prefix(path, prefix))
			return true;
		if (end == std::string::npos)
			break;
		begin = end + 1;
	}
	return false;
}

bool agent_workspace_t::resolve_existing(
    const std::string &relative, std::string &absolute, std::string &err) const
{
	return resolve_project_root(
	    user_root_, relative, absolute, err, allow_external_roots_);
}

}
}
