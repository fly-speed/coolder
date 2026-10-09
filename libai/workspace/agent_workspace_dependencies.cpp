#include "stdafx.h"
#include "agent_workspace_internal.h"

namespace webcool
{
namespace ai
{

using namespace workspace_detail;

namespace workspace_detail
{

bool dependency_contains(const std::string &parent, const std::string &path)
{
	return path == parent ||
	    (path.size() > parent.size() &&
	        path.compare(0, parent.size() + 1, parent + "/") == 0);
}
bool dependency_selected(
    const std::vector<prebuilt_dependency_t> &deps, const std::string &path)
{
	for (const auto &dep : deps) {
		if (!dependency_contains(dep.root, path))
			continue;

		if (!dep.build_system.empty())
			return true;
		for (const auto &item : dep.artifacts) {
			const std::string selected = dep.root + "/" + item;
			if (!(dependency_contains(selected, path) ||
			        dependency_contains(path, selected)))
				continue;
			return true;
		}
		return false;
	}
	return true;
}

} // namespace workspace_detail

static bool parse_dependency_build(acl::json_node *format,
    acl::json_node *array, prebuilt_dependency_t &dep, std::string &err)
{
	if (std::string(format->get_text()) != "dependencies-v2") {
		err = "source builds require dependencies-v2";
		return false;
	}
	auto *config = (*array)["build"];
	if (config && !config->is_object())
		config = config->get_obj();
	if (!config || !config->is_object()) {
		err = "expected build configuration";
		return false;
	}
	auto *system = (*config)["system"];
	dep.build_system =
	    system && system->get_text() ? system->get_text() : "auto";
	if ((system && !system->get_text()) ||
	    (dep.build_system != "auto" && dep.build_system != "cmake" &&
	        dep.build_system != "make")) {
		err =
		    "supported source dependency build systems: auto, cmake, make";
		return false;
	}
	const char *names[] = { "target", "install_target", "prefix_variable" };
	std::string *values[] = { &dep.make_target, &dep.install_target,
		&dep.prefix_variable };
	for (size_t i = 0; i < 3; ++i) {
		auto *value = (*config)[names[i]];
		if (!value)
			continue;
		dep.has_make_options = true;
		if (dep.build_system == "cmake" || !value->get_text()) {
			err =
			    "Make options require make/auto and string values";
			return false;
		}
		*values[i] = value->get_text();
		const std::string allowed = i == 2 ?
		    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_" :
		    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-";
		if (!(values[i]->empty() || values[i]->size() > 128 ||
		        (*values[i])[0] == '-' ||
		        values[i]->find_first_not_of(allowed) !=
		            std::string::npos ||
		        (i == 2 &&
		            (*values[i] == "MAKEFLAGS" ||
		                *values[i] == "MAKEFILES" ||
		                *values[i] == "SHELL" ||
		                *values[i] == "MAKE"))))
			continue;
		err = "invalid Make target or prefix_variable";
		return false;
	}
	auto *type = (*config)["type"];
	dep.has_cmake_options = type || (*config)["definitions"];
	if (dep.build_system == "make" && dep.has_cmake_options) {
		err = "Make does not support CMake type/definitions";
		return false;
	}
	if (type) {
		if (!type->get_text()) {
			err = "expected CMake build type";
			return false;
		}
		dep.build_type = type->get_text();
	}
	if (dep.build_type != "Release" && dep.build_type != "Debug" &&
	    dep.build_type != "RelWithDebInfo" &&
	    dep.build_type != "MinSizeRel") {
		err = "unsupported CMake build type";
		return false;
	}
	auto *options = (*config)["definitions"];
	if (options && !options->is_object())
		options = options->get_obj();
	if (options && !options->is_object()) {
		err = "definitions must be an object of strings";
		return false;
	}
	for (auto *option = options ? options->first_child() : NULL; option;
	     option = options->next_child()) {
		const std::string key =
		    option->tag_name() ? option->tag_name() : "";
		const std::string value =
		    option->get_text() ? option->get_text() : "";
		if (key.empty() || key.size() > 128 ||
		    key.find_first_not_of(
		        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") !=
		        std::string::npos ||
		    !option->get_text() || value.size() > 512 ||
		    value.find_first_of("\r\n") != std::string::npos ||
		    key.compare(0, 6, "CMAKE_") == 0 ||
		    dep.definitions.size() >= 32 ||
		    dep.definitions.count(key)) {
			err =
			    "invalid or reserved CMake definition (CMAKE_* is managed by WebCool)";
			return false;
		}
		dep.definitions[key] = value;
	}
	return true;
}

static bool artifact_overlaps(
    const std::vector<std::string> &artifacts, const std::string &path)
{
	for (const auto &previous : artifacts) {
		if (dependency_contains(previous, path) ||
		    dependency_contains(path, previous))
			return true;
	}
	return false;
}

bool agent_workspace_t::prebuilt_dependencies(const std::string &directory,
    std::vector<prebuilt_dependency_t> &deps, std::string &err) const
{
	deps.clear();
	std::string absolute;
	if (!resolve_existing(directory, absolute, err))
		return false;
	const std::string name = ".webcool-dependencies.json";
	struct stat st;
	if (lstat((absolute + "/" + name).c_str(), &st) != 0) {
		if (errno == ENOENT)
			return true;
		err = "cannot inspect prebuilt dependency manifest";
		return false;
	}
	std::string text;
	bool truncated = false;
	if (!read(directory.empty() ? name : directory + "/" + name, text,
	        truncated, err) ||
	    truncated)
		return false;
	acl::json json(text.c_str());
	auto fail = [&]() {
		err +=
		    " invalid .webcool-dependencies.json: expected prebuilt-v1/dependencies-v2, project-relative roots and artifact paths";
		return false;
	};
	if (!json.finish()) {
		err = "invalid JSON";
		return fail();
	}
	auto *format = json.get_root()["format"];
	auto *items = json.get_root()["dependencies"];
	if (items && !items->is_object())
		items = items->get_obj();
	if (!format || !format->get_text() ||
	    (std::string(format->get_text()) != "prebuilt-v1" &&
	        std::string(format->get_text()) != "dependencies-v2") ||
	    !items || !items->is_object()) {
		err =
		    "expected format prebuilt-v1/dependencies-v2 and dependencies object";
		return fail();
	}
	for (auto *node = items->first_child(); node;
	     node = items->next_child()) {
		prebuilt_dependency_t dep;
		auto *array = node->is_array() ? node : node->get_obj();
		if (array && array->is_object()) {
			if (!parse_dependency_build(format, array, dep, err))
				return fail();
			array = (*array)["artifacts"];
			if (array && !array->is_array())
				array = array->get_obj();
		}
		if (!node->tag_name() || !array || !array->is_array()) {
			err = "expected artifact array";
			return fail();
		}
		dep.root = node->tag_name();
		std::string normalized;
		if (!normalize_path(dep.root, normalized, false, err) ||
		    normalized != dep.root || path_is_sensitive(dep.root)) {
			err = "unsafe dependency root";
			return fail();
		}
		for (const auto &previous : deps) {
			if (!(dependency_contains(previous.root, dep.root) ||
			        dependency_contains(dep.root, previous.root)))
				continue;

			err = "overlapping dependency roots";
			return fail();
		}
		for (auto *item = array->first_child(); item;
		     item = array->next_child()) {
			if (!item->get_text()) {
				err = "expected artifact path string";
				return fail();
			}
			const std::string path = item->get_text();
			if (!normalize_path(path, normalized, false, err) ||
			    normalized != path ||
			    path_is_sensitive(dep.root + "/" + path)) {
				err = "unsafe artifact path";
				return fail();
			}
			if (artifact_overlaps(dep.artifacts, path)) {
				err = "overlapping artifact paths";
				return fail();
			}
			dep.artifacts.push_back(path);
			if (!(dep.artifacts.size() > 64))
				continue;
			err = "more than 64 artifact paths";
			return fail();
		}
		if (dep.artifacts.empty() || deps.size() >= 8) {
			err = "empty artifact list or more than 8 dependencies";
			return fail();
		}
		deps.push_back(dep);
	}
	return true;
}

bool agent_workspace_t::copy_build_dependency(const std::string &source_file,
    const agent_workspace_t &destination, const std::string &destination_file,
    std::string &err) const
{
	std::string source, relative, parent;
	if (!resolve_existing(source_file, source, err) ||
	    !normalize_path(destination_file, relative, false, err))
		return false;
	const size_t slash = relative.rfind('/');
	if (!destination.resolve_existing(
	        slash == std::string::npos ? "" : relative.substr(0, slash),
	        parent, err))
		return false;
	const std::string target = join_path(parent,
	    slash == std::string::npos ? relative : relative.substr(slash + 1));
#ifdef _WIN32
	std::wstring source_wide, target_wide;
	if (!webcool_utf8_path_to_wide(source.c_str(), source_wide) ||
	    !webcool_utf8_path_to_wide(target.c_str(), target_wide)) {
		err = "cannot encode build dependency path";
		return false;
	}
	const DWORD attr = GetFileAttributesW(source_wide.c_str());
	if (attr == INVALID_FILE_ATTRIBUTES ||
	    (attr &
	        (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
		err = "build dependency is not a regular file";
		return false;
	}
	if (CopyFileW(source_wide.c_str(), target_wide.c_str(), TRUE))
		return true;
	err = "cannot copy build dependency: " + source_file;
	return false;
#else
	const int in = open(source.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
	struct stat st;
	if (in < 0) {
		err = "cannot open build dependency: " + source_file;
		return false;
	}
	if (fstat(in, &st) != 0 || !S_ISREG(st.st_mode) ||
	    st.st_size > 512LL * 1024 * 1024) {
		close(in);
		err =
		    "build dependency must be a regular file no larger than 512 MiB: " +
		    source_file;
		return false;
	}
#ifdef __APPLE__
	// Independent copy-on-write bytes; unlike hard links, draft writes cannot
	// modify the user's prebuilt library. Fall back on other filesystems.
	if (fclonefileat(in, AT_FDCWD, target.c_str(), 0) == 0) {
		close(in);
		return true;
	}
#endif
	const int out = open(
	    target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	if (out < 0) {
		close(in);
		err = "cannot create private build dependency: " +
		    destination_file;
		return false;
	}
	bool ok = true;
	char buffer[64 * 1024];
	long long total = 0;
	fingerprint_budget_t copy_budget;
	while (ok) {
		copy_budget.checkpoint();
		const ssize_t count = ::read(in, buffer, sizeof(buffer));
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0) {
			ok = false;
			break;
		}
		if (count == 0)
			break;
		total += count;
		if (total > 512LL * 1024 * 1024) {
			ok = false;
			break;
		}
		ssize_t offset = 0;
		while (offset < count) {
			const ssize_t written =
			    ::write(out, buffer + offset, count - offset);
			if (written < 0 && errno == EINTR)
				continue;
			if (written <= 0) {
				ok = false;
				break;
			}
			offset += written;
		}
	}
	if (fchmod(out, st.st_mode & 0777) != 0)
		ok = false;
	if (close(out) != 0)
		ok = false;
	close(in);
	if (!ok) {
		unlink(target.c_str());
		err = "cannot copy private build dependency: " + source_file;
	}
	return ok;
#endif
}

#ifndef _WIN32
namespace
{
struct build_tree_copy_t {
	std::vector<std::string> excluded;
	std::string logical_root;
	size_t skipped = 0, files = 0, directories = 0;
	size_t cloned_files = 0, streamed_files = 0;
	long long streamed_bytes = 0;
	long long dependency_bytes = 0;
	const std::function<void()> &checkpoint;
	explicit build_tree_copy_t(const std::function<void()> &callback)
	        : checkpoint(callback)
	{
	}
	bool file(int source, int target, const std::string &name,
	    const struct stat &expected, std::string &err);
	bool directory(int source, int target, const std::string &relative,
	    size_t depth, std::string &err);
};

static bool write_private_copy_buffer(
    int destination, const char *buffer, ssize_t count, std::string &err)
{
	ssize_t offset = 0;
	while (offset < count) {
		const ssize_t written =
		    ::write(destination, buffer + offset, count - offset);
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0) {
			err = "cannot write private copy destination";
			return false;
		}
		offset += written;
	}

	return true;
}

inline bool build_tree_copy_t::file(int source, int target,
    const std::string &name, const struct stat &expected, std::string &err)
{
	const size_t dot = name.rfind('.');
	const std::string ext =
	    dot == std::string::npos ? "" : name.substr(dot);
	const bool library = ext == ".a" || ext == ".lib" || ext == ".so" ||
	    ext == ".dylib" || ext == ".dll" ||
	    name.find(".so.") != std::string::npos;
	if (!library && expected.st_size > 1024 * 1024) {
		++skipped;
		return true;
	}
	if (library) {
		dependency_bytes += expected.st_size;
		if (expected.st_size > 512LL * 1024 * 1024 ||
		    dependency_bytes > 2LL * 1024 * 1024 * 1024) {
			err = "private build dependencies exceed size limit";
			return false;
		}
	}
	fingerprint_fd_t input(openat(source, name.c_str(),
	    O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
	struct stat before, after;
	if (input.fd < 0 || fstat(input.fd, &before) != 0 ||
	    !S_ISREG(before.st_mode) ||
	    fingerprint_stat(before) != fingerprint_stat(expected)) {
		err = "source changed before private copy";
		return false;
	}
	const auto cached = fingerprint_cache.find(fingerprint_stat(before));
	if (!library && cached != fingerprint_cache.end() &&
	    cached->second.binary_prefix) {
		++skipped;
		return true;
	}
	if (!library && cached == fingerprint_cache.end()) {
		char probe[8192];
		size_t total = 0;
		const size_t wanted = std::min<size_t>(
		    sizeof(probe), static_cast<size_t>(expected.st_size));
		while (total < wanted) {
			const ssize_t count = pread(
			    input.fd, probe + total, wanted - total, total);
			if (count < 0 && errno == EINTR)
				continue;
			if (count <= 0) {
				err = "cannot inspect private copy source";
				return false;
			}
			total += static_cast<size_t>(count);
		}
		if (memchr(probe, 0, total)) {
			++skipped;
			return true;
		}
	}
	bool copied = false;
#ifdef __APPLE__
	// Cloning thousands of tiny headers pays filesystem clone metadata
	// overhead per file. Stream small files; reserve COW clones for large
	// dependencies where avoiding their data copy actually pays off.
	if (expected.st_size > 64 * 1024)
		copied = fclonefileat(input.fd, target, name.c_str(), 0) == 0;
#endif
	if (copied)
		++cloned_files;
	if (!copied) {
		fingerprint_fd_t output(openat(target, name.c_str(),
		    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
		    0600));
		if (output.fd < 0) {
			err = "cannot create private copy destination";
			return false;
		}
		char buffer[65536];
		long long total = 0;
		while (true) {
			checkpoint();
			const ssize_t count =
			    ::read(input.fd, buffer, sizeof(buffer));
			if (count < 0 && errno == EINTR)
				continue;
			if (count < 0) {
				err = "cannot read private copy source";
				return false;
			}
			if (count == 0)
				break;
			total += count;
			if (total > expected.st_size) {
				err = "private copy source grew";
				return false;
			}
			if (!write_private_copy_buffer(
			        output.fd, buffer, count, err))
				return false;
		}
		if (total != expected.st_size ||
		    fchmod(output.fd, expected.st_mode & 0777) != 0) {
			err = "private copy size or mode mismatch";
			return false;
		}
		const int close_result = close(output.fd);
		output.fd = -1;
		if (close_result != 0) {
			err = "cannot close private copy destination";
			return false;
		}
		++streamed_files;
		streamed_bytes += total;
	}
	if (!(fstat(input.fd, &after) != 0 ||
	        fingerprint_stat(after) != fingerprint_stat(expected)))
		return true;
	err = "source changed during private copy";
	return false;
}

inline bool build_tree_copy_t::directory(int source, int target,
    const std::string &relative, size_t depth, std::string &err)
{
	if (++directories > 16384 || depth > 128) {
		err = "private copy directory limit";
		return false;
	}
	struct stat before, after;
	if (fstat(source, &before) != 0) {
		err = "cannot inspect private copy directory";
		return false;
	}
	const int listing_fd = dup(source);
	DIR *listing = listing_fd < 0 ? NULL : fdopendir(listing_fd);
	if (!listing) {
		if (listing_fd >= 0)
			close(listing_fd);
		err = "cannot enumerate private copy source";
		return false;
	}
	std::vector<std::string> names;
	bool complete = true;
	while (true) {
		errno = 0;
		const dirent *entry = readdir(listing);
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
		if (agent_workspace_t::path_is_sensitive(logical_root.empty() ?
		            path :
		            logical_root + "/" + path))
			continue;
		if (std::find(excluded.begin(), excluded.end(), path) !=
		    excluded.end())
			continue;
		names.push_back(name);
		checkpoint();
		if (!(names.size() >= 2000))
			continue;
		complete = false;
		break;
	}
	closedir(listing);
	if (!complete) {
		err = "private copy directory incomplete or exceeds limit";
		return false;
	}
	std::sort(names.begin(), names.end());
	for (const auto &name : names) {
		checkpoint();
		struct stat st;
		if (fstatat(source, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) !=
		    0) {
			err = "private copy entry disappeared";
			return false;
		}
		if (S_ISLNK(st.st_mode))
			continue;
		if (S_ISDIR(st.st_mode)) {
			fingerprint_fd_t input(openat(source, name.c_str(),
			    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
			if (input.fd < 0 ||
			    mkdirat(target, name.c_str(), 0700) != 0) {
				err = "cannot create private copy directory";
				return false;
			}
			fingerprint_fd_t output(openat(target, name.c_str(),
			    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
			if (!(output.fd < 0 ||
			        !directory(input.fd, output.fd,
			            relative.empty() ? name :
			                               relative + "/" + name,
			            depth + 1, err)))
				continue;
			return false;
		} else if (S_ISREG(st.st_mode)) {
			if (++files > 100000) {
				err = "private copy file count limit";
				return false;
			}
			if (!file(source, target, name, st, err))
				return false;
		} else {
			++skipped;
		}
	}
	if (!(fstat(source, &after) != 0 ||
	        fingerprint_stat(before) != fingerprint_stat(after)))
		return true;
	err = "source directory changed during private copy";
	return false;
}

}

bool agent_workspace_t::copy_build_tree_to(const std::string &source_directory,
    const agent_workspace_t &destination, size_t &skipped_files,
    long long &dependency_bytes, const std::function<void()> &checkpoint,
    std::string &err, const std::vector<std::string> &excluded) const
{
	std::string normalized, source, target;
	if (!checkpoint ||
	    !normalize_path(source_directory, normalized, true, err) ||
	    !resolve_existing(normalized, source, err) ||
	    !destination.resolve_existing("", target, err))
		return false;
	fingerprint_fd_t input(open(
	    source.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
	fingerprint_fd_t output(open(
	    target.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
	if (input.fd < 0 || output.fd < 0) {
		err = "cannot open private copy roots";
		return false;
	}
	build_tree_copy_t copy(checkpoint);
	copy.logical_root = normalized;
	copy.excluded = excluded;
	const bool ok = copy.directory(input.fd, output.fd, "", 0, err);
	skipped_files = copy.skipped;
	dependency_bytes = copy.dependency_bytes;
	logger(
	    "AI component=agent.workspace event=copy_strategy ok=%d cloned_files=%zu streamed_files=%zu streamed_bytes=%lld skipped_files=%zu",
	    ok, copy.cloned_files, copy.streamed_files, copy.streamed_bytes,
	    copy.skipped);
	return ok;
}
#endif

}
}
