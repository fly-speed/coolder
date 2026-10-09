#include "stdafx.h"
#include "agent_workspace_internal.h"

namespace webcool
{
namespace ai
{

using namespace workspace_detail;

namespace workspace_detail
{

#ifndef _WIN32
std::string fingerprint_stat(const struct stat &value)
{
#ifdef __APPLE__
	const auto mt = value.st_mtimespec, ct = value.st_ctimespec;
#else
	const auto mt = value.st_mtim, ct = value.st_ctim;
#endif
	return std::to_string(value.st_dev) + ":" +
	       std::to_string(value.st_ino) + ":" +
	       std::to_string(value.st_mode) + ":" +
	       std::to_string(value.st_size) + ":" + std::to_string(mt.tv_sec) +
	       ":" + std::to_string(mt.tv_nsec) + ":" +
	       std::to_string(ct.tv_sec) + ":" + std::to_string(ct.tv_nsec);
}

thread_local std::map<std::string, fingerprint_record_t> fingerprint_cache;
#endif

} // namespace workspace_detail
namespace
{

#ifndef _WIN32
struct fingerprint_scan_t {
	size_t files = 0, directories = 0;
	long long bytes = 0;
	std::string evidence, logical_root;
	std::vector<prebuilt_dependency_t> dependencies;
	std::vector<std::string> included_paths;
	fingerprint_budget_t budget;
	bool file(int parent, const std::string &name,
		  const struct stat &expected, std::string &digest,
		  std::string &err)
	{
		if (expected.st_size > 512LL * 1024 * 1024) {
			err = "fingerprint file size limit";
			return false;
		}
		auto &cache = fingerprint_cache;
		const std::string identity = fingerprint_stat(expected);
#ifdef __APPLE__
		const bool precise = expected.st_ctimespec.tv_nsec != 0;
#else
		const bool precise = expected.st_ctim.tv_nsec != 0;
#endif
		const auto found = cache.find(identity);
		if (precise && found != cache.end()) {
			digest = found->second.digest;
			return true;
		}
		fingerprint_fd_t input(
			openat(parent, name.c_str(),
			       O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
		struct stat before, after;
		if (input.fd < 0 || fstat(input.fd, &before) != 0 ||
		    !S_ISREG(before.st_mode) ||
		    fingerprint_stat(before) != identity) {
			err = "workspace file changed before fingerprint";
			return false;
		}
		EVP_MD_CTX *context = EVP_MD_CTX_new();
		if (!context) {
			err = "cannot allocate fingerprint context";
			return false;
		}
		bool ok = EVP_DigestInit_ex(context, EVP_sha256(), NULL) == 1;
		char buffer[65536];
		long long total = 0;
		bool binary_prefix = false;
		while (ok) {
			budget.checkpoint();
			const ssize_t count =
				::read(input.fd, buffer, sizeof(buffer));
			if (count < 0 && errno == EINTR)
				continue;
			if (count < 0) {
				ok = false;
				break;
			}
			if (count == 0)
				break;
			if (total < 8192 &&
			    memchr(buffer, 0,
				   std::min<size_t>(
					   static_cast<size_t>(count),
					   8192 - static_cast<size_t>(total))))
				binary_prefix = true;
			total += count;
			ok = total <= expected.st_size &&
			     EVP_DigestUpdate(context, buffer,
					      static_cast<size_t>(count)) == 1;
		}
		unsigned char hash[EVP_MAX_MD_SIZE];
		unsigned int length = 0;
		ok = ok && total == expected.st_size &&
		     fstat(input.fd, &after) == 0 &&
		     fingerprint_stat(after) == identity &&
		     EVP_DigestFinal_ex(context, hash, &length) == 1;
		EVP_MD_CTX_free(context);
		if (!ok) {
			err = "workspace file changed during fingerprint";
			return false;
		}
		const char *hex = "0123456789abcdef";
		digest.clear();
		for (unsigned int i = 0; i < length; ++i) {
			digest += hex[hash[i] >> 4];
			digest += hex[hash[i] & 15];
		}
		if (precise) {
			if (cache.size() >= 32768)
				cache.clear();
			auto &record = cache[identity];
			record.digest = digest;
			record.binary_prefix = binary_prefix;
		}
		return true;
	}
	bool directory(int fd, const std::string &relative, size_t depth,
		       std::string &err)
	{
		if (++directories > 16384 || depth > 128) {
			err = "fingerprint directory limit";
			return false;
		}
		struct stat before, after;
		if (fstat(fd, &before) != 0)
			return false;
		DIR *stream = fdopendir(dup(fd));
		if (!stream) {
			err = "cannot enumerate fingerprint directory";
			return false;
		}
		std::vector<std::string> names;
		bool complete = true;
		while (true) {
			errno = 0;
			dirent *entry = readdir(stream);
			if (!entry) {
				complete = errno == 0;
				break;
			}
			const std::string name = entry->d_name;
			if (name == "." || name == ".." ||
			    (relative.empty() && name == ".webcool-build"))
				continue;
			const std::string path =
				relative.empty() ? name : relative + "/" + name;
			const std::string logical =
				logical_root.empty() ?
					path :
					logical_root + "/" + path;
			if (agent_workspace_t::path_is_sensitive(logical) ||
			    !dependency_selected(dependencies, path))
				continue;
			if (!included_paths.empty()) {
				bool selected = false;
				for (const auto &included : included_paths)
					if (dependency_contains(included,
								path) ||
					    dependency_contains(path,
								included)) {
						selected = true;
						break;
					}
				if (!selected)
					continue;
			}
			names.push_back(name);
			budget.checkpoint();
			if (names.size() >= 2000) {
				complete = false;
				break;
			}
		}
		closedir(stream);
		if (!complete) {
			err = "fingerprint directory incomplete or exceeds limit";
			return false;
		}
		std::sort(names.begin(), names.end());
		for (const auto &name : names) {
			budget.checkpoint();
			struct stat st;
			if (fstatat(fd, name.c_str(), &st,
				    AT_SYMLINK_NOFOLLOW) != 0) {
				err = "workspace entry changed during fingerprint";
				return false;
			}
			if (S_ISLNK(st.st_mode))
				continue;
			const std::string path =
				relative.empty() ? name : relative + "/" + name;
			evidence +=
				std::to_string(path.size()) + ":" + path + ":" +
				std::to_string(
					!included_paths.empty() &&
							S_ISDIR(st.st_mode) ?
						0 :
						st.st_mode & 0777) +
				"\n";
			if (S_ISDIR(st.st_mode)) {
				fingerprint_fd_t child(
					openat(fd, name.c_str(),
					       O_RDONLY | O_DIRECTORY |
						       O_NOFOLLOW | O_CLOEXEC));
				if (child.fd < 0 ||
				    !directory(child.fd, path, depth + 1, err))
					return false;
			} else {
				bytes += st.st_size;
				if (!S_ISREG(st.st_mode) ||
				    ++files > (included_paths.empty() ?
						       100000 :
						       20000) ||
				    bytes > (included_paths.empty() ? 4LL :
								      2LL) *
						    1024 * 1024 * 1024) {
					err = "fingerprint file limit or unsafe entry";
					return false;
				}
				std::string hash;
				if (!file(fd, name, st, hash, err))
					return false;
				evidence += hash + "\n";
			}
		}
		if (fstat(fd, &after) != 0 ||
		    fingerprint_stat(before) != fingerprint_stat(after)) {
			err = "workspace directory changed during fingerprint";
			return false;
		}
		return true;
	}
};
#endif

} // namespace

bool agent_workspace_t::tree_sha256(
	const std::string &directory, std::string &digest, std::string &err,
	const std::vector<std::string> &included_paths) const
{
	digest.clear();
#ifndef _WIN32
	std::string normalized, absolute;
	if (!normalize_path(directory, normalized, true, err) ||
	    !resolve_existing(normalized, absolute, err))
		return false;
	fingerprint_fd_t root(open(absolute.c_str(), O_RDONLY | O_DIRECTORY |
							     O_NOFOLLOW |
							     O_CLOEXEC));
	if (root.fd < 0) {
		err = "cannot open fingerprint root";
		return false;
	}
	fingerprint_scan_t scan;
	scan.logical_root = normalized;
	scan.included_paths = included_paths;
	if (!prebuilt_dependencies(directory, scan.dependencies, err))
		return false;
	if (!scan.directory(root.fd, "", 0, err))
		return false;
	digest = content_sha256(scan.evidence);
	return true;
#else
	(void)included_paths;
	fingerprint_budget_t budget;
	std::vector<std::string> directories(1, directory);
	std::string evidence;
	size_t files = 0;
	long long bytes = 0;
	for (size_t i = 0; i < directories.size(); ++i) {
		if (directories.size() > 16384) {
			err = "fingerprint directory limit";
			return false;
		}
		std::vector<workspace_entry_t> entries;
		if (!list(directories[i], entries, err) ||
		    entries.size() >= 2000)
			return false;
		for (const auto &entry : entries) {
			budget.checkpoint();
			const std::string relative =
				directory.empty() ?
					entry.path :
					entry.path.substr(directory.size() + 1);
			// Private build output is disposable, never part of the source baseline.
			if (relative == ".webcool-build")
				continue;
			evidence += std::to_string(relative.size()) + ":" +
				    relative + "\n";
			if (entry.directory) {
				directories.push_back(entry.path);
				continue;
			}
			bytes += entry.size;
			if (++files > 100000 ||
			    bytes > 4LL * 1024 * 1024 * 1024) {
				err = "fingerprint file limit";
				return false;
			}
			std::string hash;
			if (!file_sha256(entry.path, hash, err))
				return false;
			evidence += hash + "\n";
		}
	}
	digest = content_sha256(evidence);
	return true;
#endif
}

bool agent_workspace_t::file_sha256(const std::string &relative_file,
				    std::string &digest, std::string &err) const
{
	digest.clear();
	std::string absolute;
	if (!resolve_existing(relative_file, absolute, err))
		return false;
	struct stat st;
	if (lstat(absolute.c_str(), &st) != 0 || !S_ISREG(st.st_mode) ||
	    is_link_like(absolute) || st.st_size > 512LL * 1024 * 1024) {
		err = "cannot fingerprint regular workspace file";
		return false;
	}
#ifndef _WIN32
	// ctime catches same-size writes even when the writer restores mtime.
	// Cache is thread-local, bounded, and never replaces path/containment checks.
	auto stamp = [](const struct stat &value) {
#ifdef __APPLE__
		const auto mt = value.st_mtimespec, ct = value.st_ctimespec;
#else
		const auto mt = value.st_mtim, ct = value.st_ctim;
#endif
		return std::to_string(value.st_dev) + ":" +
		       std::to_string(value.st_ino) + ":" +
		       std::to_string(value.st_size) + ":" +
		       std::to_string(mt.tv_sec) + ":" +
		       std::to_string(mt.tv_nsec) + ":" +
		       std::to_string(ct.tv_sec) + ":" +
		       std::to_string(ct.tv_nsec);
	};
	static thread_local std::map<std::string,
				     std::pair<std::string, std::string>>
		cached;
	const std::string identity = stamp(st);
	const auto found = cached.find(absolute);
	if (found != cached.end() && found->second.first == identity) {
		digest = found->second.second;
		return true;
	}
#endif
	std::ifstream input(absolute.c_str(), std::ios::binary);
	if (!input) {
		err = "cannot open workspace file for fingerprint";
		return false;
	}
	EVP_MD_CTX *context = EVP_MD_CTX_new();
	if (!context) {
		err = "cannot allocate fingerprint context";
		return false;
	}
	bool ok = EVP_DigestInit_ex(context, EVP_sha256(), NULL) == 1;
	fingerprint_budget_t budget;
	char buffer[65536];
	long long bytes = 0;
	while (ok && input) {
		budget.checkpoint();
		input.read(buffer, sizeof(buffer));
		const std::streamsize count = input.gcount();
		bytes += count;
		ok = bytes <= 512LL * 1024 * 1024 &&
		     EVP_DigestUpdate(context, buffer,
				      static_cast<size_t>(count)) == 1;
	}
	unsigned char hash[EVP_MAX_MD_SIZE];
	unsigned int length = 0;
	ok = ok && input.eof() && bytes == st.st_size &&
	     EVP_DigestFinal_ex(context, hash, &length) == 1;
	EVP_MD_CTX_free(context);
	if (!ok) {
		err = "workspace fingerprint read failed or file changed";
		return false;
	}
	const char *hex = "0123456789abcdef";
	for (unsigned int i = 0; i < length; ++i) {
		digest += hex[hash[i] >> 4];
		digest += hex[hash[i] & 15];
	}
#ifndef _WIN32
	struct stat after;
	if (lstat(absolute.c_str(), &after) != 0 || stamp(after) != identity) {
		digest.clear();
		err = "workspace changed during fingerprint";
		return false;
	}
	if (cached.size() >= 32768)
		cached.clear();
	cached[absolute] = std::make_pair(identity, digest);
#endif
	return true;
}

}
}
