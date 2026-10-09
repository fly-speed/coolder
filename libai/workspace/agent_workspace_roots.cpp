#include "stdafx.h"
#include "agent_workspace_internal.h"

namespace webcool
{
namespace ai
{

using namespace workspace_detail;

namespace
{

std::mutex g_project_root_registry_mutex;

std::string registry_path(const std::string &user_root)
{
	return user_root + "/.webcool_agent/external-project-roots.v1";
}

using ::webcool::ai::record_codec::hex_encode;

using ::webcool::ai::record_codec::hex_value;

bool hex_decode(const std::string &value, std::string &decoded)
{
	if (!(value.size() % 2 != 0))
		return ::webcool::ai::record_codec::hex_decode(value, decoded);
	return false;
}

#ifndef _WIN32
std::string registry_identity(const struct stat &value)
{
#ifdef __APPLE__
	const auto mt = value.st_mtimespec, ct = value.st_ctimespec;
#else
	const auto mt = value.st_mtim, ct = value.st_ctim;
#endif
	return std::to_string(value.st_dev) + ":" +
	    std::to_string(value.st_ino) + ":" + std::to_string(value.st_size) +
	    ":" + std::to_string(mt.tv_sec) + ":" + std::to_string(mt.tv_nsec) +
	    ":" + std::to_string(ct.tv_sec) + ":" + std::to_string(ct.tv_nsec);
}
#endif

bool read_project_roots(const std::string &user_root,
    std::map<std::string, std::string> &roots, std::string &err)
{
	roots.clear();
#ifndef _WIN32
	// All callers hold g_project_root_registry_mutex. Revalidate metadata on
	// EVERY access, including after external edits; no time-based stale window.
	typedef std::pair<std::string, std::map<std::string, std::string>>
	    cached_roots_t;
	static std::map<std::string, cached_roots_t> cache;
	struct stat before;
	const bool present =
	    stat(registry_path(user_root).c_str(), &before) == 0;
#ifdef __APPLE__
	const bool precise = present && before.st_ctimespec.tv_nsec != 0;
#else
	const bool precise = present && before.st_ctim.tv_nsec != 0;
#endif
	const std::string identity = present ? registry_identity(before) : "";
	const auto hit = cache.find(user_root);
	if (precise && hit != cache.end() && hit->second.first == identity) {
		roots = hit->second.second;
		return true;
	}
	cache.erase(user_root);
#endif
	std::ifstream in(registry_path(user_root).c_str(), std::ios::in);
	if (!in.good())
		return true;
	std::string line;
	while (std::getline(in, line)) {
		const size_t tab = line.find('\t');
		std::string logical;
		std::string physical;
		if (tab == std::string::npos ||
		    !hex_decode(line.substr(0, tab), logical) ||
		    !hex_decode(line.substr(tab + 1), physical)) {
			err = "invalid external project root registry";
			return false;
		}
		roots[logical] = physical;
	}
	if (in.bad()) {
		err = "cannot read external project root registry";
		return false;
	}
#ifndef _WIN32
	struct stat after;
	if (precise && roots.size() <= 4096 &&
	    stat(registry_path(user_root).c_str(), &after) == 0 &&
	    registry_identity(after) == identity) {
		if (cache.size() >= 32)
			cache.clear();
		cache[user_root] = std::make_pair(identity, roots);
	}
#endif

	return true;
}

bool make_private_directory_if_needed(const std::string &path)
{
#ifdef _WIN32
	std::wstring wide;
	if (!webcool_utf8_path_to_wide(path.c_str(), wide))
		return false;
	const DWORD attrs = GetFileAttributesW(wide.c_str());
	if (attrs != INVALID_FILE_ATTRIBUTES)
		return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
		    (attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
	return _wmkdir(wide.c_str()) == 0;
#else
	struct stat st;
	if (lstat(path.c_str(), &st) == 0)
		return S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);
	return errno == ENOENT && mkdir(path.c_str(), 0700) == 0;
#endif
}

} // namespace

bool agent_workspace_t::register_project_root(const std::string &user_root,
    const std::string &logical_path, const std::string &absolute_root,
    std::string &err)
{
	std::string logical;
	if (!normalize_path(logical_path, logical, false, err))
		return false;
	char resolved[PATH_MAX];
	if (realpath(absolute_root.c_str(), resolved) == NULL) {
		err = "external project directory does not exist";
		return false;
	}
	const std::string physical(resolved);
	if (physical == "/" ||
	    inspect_path_component(physical) != PATH_COMPONENT_NORMAL) {
		err = "external project root is not a safe directory";
		return false;
	}
	struct stat st;
	if (stat(physical.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
		err = "external project root is not a directory";
		return false;
	}
	std::lock_guard<std::mutex> guard(g_project_root_registry_mutex);
	std::map<std::string, std::string> roots;
	if (!read_project_roots(user_root, roots, err))
		return false;
	roots[logical] = physical;
	const std::string metadata = user_root + "/.webcool_agent";
	if (!make_private_directory_if_needed(metadata)) {
		err = "cannot create external project root registry directory";
		return false;
	}
	const std::string target = registry_path(user_root);
	const std::string temporary = target + ".tmp";
	std::ofstream out(temporary.c_str(), std::ios::out | std::ios::trunc);
	if (!out.good()) {
		err = "cannot write external project root registry";
		return false;
	}
	for (std::map<std::string, std::string>::const_iterator it =
	         roots.begin();
	    it != roots.end(); ++it) {
		out << hex_encode(it->first) << '\t' << hex_encode(it->second)
		    << '\n';
	}
	out.close();
	if (!out.good()) {
		remove(temporary.c_str());
		err = "cannot flush external project root registry";
		return false;
	}
#ifndef _WIN32
	if (chmod(temporary.c_str(), 0600) != 0) {
		remove(temporary.c_str());
		err = "cannot protect external project root registry";
		return false;
	}
#endif
	if (replace_file(temporary, target))
		return true;
	remove(temporary.c_str());
	err = "cannot install external project root registry";
	return false;
}

bool agent_workspace_t::resolve_project_root(const std::string &user_root,
    const std::string &logical_path, std::string &absolute_root,
    std::string &err, bool allow_external_roots)
{
	std::string logical;
	if (!normalize_path(logical_path, logical, true, err))
		return false;
	std::string mapped_logical;
	std::string mapped_physical;
	if (allow_external_roots) {
		std::lock_guard<std::mutex> guard(
		    g_project_root_registry_mutex);
		std::map<std::string, std::string> roots;
		if (!read_project_roots(user_root, roots, err))
			return false;
		for (std::map<std::string, std::string>::const_iterator it =
		         roots.begin();
		    it != roots.end(); ++it) {
			if (!(path_has_prefix(logical, it->first) &&
			        it->first.size() > mapped_logical.size()))
				continue;
			mapped_logical = it->first;
			mapped_physical = it->second;
		}
	}
	const std::string suffix = mapped_logical.empty() ?
	    logical :
	    (logical.size() == mapped_logical.size() ?
	            "" :
	            logical.substr(mapped_logical.size() + 1));
	const std::string base =
	    mapped_logical.empty() ? user_root : mapped_physical;
	const path_component_state_t state =
	    inspect_path_components(base, suffix);
	if (state == PATH_COMPONENT_LINK) {
		err = "project path contains a symbolic link or reparse point";
		return false;
	}
	if (state != PATH_COMPONENT_NORMAL) {
		err = state == PATH_COMPONENT_MISSING ?
		    "workspace path does not exist" :
		    "cannot inspect project path attributes";
		return false;
	}
	char base_path[PATH_MAX];
	char candidate_path[PATH_MAX];
	const std::string candidate =
	    suffix.empty() ? base : join_path(base, suffix);
	if (realpath(base.c_str(), base_path) == NULL ||
	    realpath(candidate.c_str(), candidate_path) == NULL ||
	    !path_is_within(base_path, candidate_path)) {
		err = "project path resolves outside its approved directory";
		return false;
	}
	absolute_root = candidate_path;
	return true;
}

bool agent_workspace_t::unregister_project_root(const std::string &user_root,
    const std::string &logical_path, std::string &err)
{
	std::string logical;
	if (!normalize_path(logical_path, logical, false, err))
		return false;
	std::lock_guard<std::mutex> guard(g_project_root_registry_mutex);
	std::map<std::string, std::string> roots;
	if (!read_project_roots(user_root, roots, err))
		return false;
	if (roots.erase(logical) == 0)
		return true;
	const std::string target = registry_path(user_root);
	const std::string temporary = target + ".tmp";
	std::ofstream out(temporary.c_str(), std::ios::out | std::ios::trunc);
	if (!out.good()) {
		err = "cannot write external project root registry";
		return false;
	}
	for (std::map<std::string, std::string>::const_iterator it =
	         roots.begin();
	    it != roots.end(); ++it) {
		out << hex_encode(it->first) << '\t' << hex_encode(it->second)
		    << '\n';
	}
	out.close();
#ifndef _WIN32
	if (!out.good() || chmod(temporary.c_str(), 0600) != 0) {
#else
	if (!out.good()) {
#endif
		remove(temporary.c_str());
		err = "cannot flush external project root registry";
		return false;
	}
	if (replace_file(temporary, target))
		return true;
	remove(temporary.c_str());
	err = "cannot install external project root registry";
	return false;
}

bool agent_workspace_t::resolve_project_state_root(const std::string &user_root,
    const std::string &logical_path, std::string &absolute_root,
    std::string &err)
{
	std::string project_root;
	if (!resolve_project_root(user_root, logical_path, project_root, err)) {
		return false;
	}
	const std::string shared_prefix = "共享目录/";
	if (logical_path.compare(0, shared_prefix.size(), shared_prefix) == 0) {
		absolute_root = project_root;
		return true;
	}
	char resolved_user[PATH_MAX];
	if (realpath(user_root.c_str(), resolved_user) == NULL) {
		err = "cannot resolve user workspace";
		return false;
	}
	if (path_is_within(resolved_user, project_root)) {
		absolute_root = project_root;
		return true;
	}
	const std::string metadata =
	    std::string(resolved_user) + "/.webcool_agent";
	const std::string states = metadata + "/external-project-state";
	const std::string digest = content_sha256(logical_path);
	if (digest.empty() || !make_private_directory_if_needed(metadata) ||
	    !make_private_directory_if_needed(states)) {
		err = "cannot create private external project state directory";
		return false;
	}
	absolute_root = states + "/" + digest;
	if (!make_private_directory_if_needed(absolute_root)) {
		err = "cannot create private external project state root";
		return false;
	}
#ifndef _WIN32
	if (chmod(states.c_str(), 0700) != 0 ||
	    chmod(absolute_root.c_str(), 0700) != 0) {
		err = "cannot protect private external project state root";
		return false;
	}
#endif
	return true;
}

}
}
