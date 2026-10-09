#include "stdafx.h"
#include "agent_draft_store_internal.h"
namespace webcool
{
namespace ai
{
namespace draft_store_detail
{
bool dep_inside(const std::string &parent, const std::string &path)
{
	return path == parent ||
	    path.compare(0, parent.size() + 1, parent + "/") == 0;
}
bool dependency_files(agent_workspace_t &workspace, const std::string &path,
    std::vector<std::string> &files, size_t depth, draft_io_budget_t &budget,
    std::string &err, std::vector<std::string> &directories)
{
	if (depth > 64 || files.size() >= 20000) {
		err = "prebuilt dependency exceeds file/depth limit";
		return false;
	}
	budget.checkpoint();
	std::vector<workspace_entry_t> entries;
	if (!workspace.list(path, entries, err))
		return false;
	if (entries.size() >= 2000) {
		err = "prebuilt dependency directory listing exceeds limit";
		return false;
	}
	for (const auto &entry : entries) {
		if (entry.directory) {
			if (directories.size() >= 16384) {
				err =
				    "prebuilt dependency exceeds directory limit";
				return false;
			}
			directories.push_back(entry.path);
			if (dependency_files(workspace, entry.path, files,
			        depth + 1, budget, err, directories))
				continue;
			return false;
		} else {
			if (files.size() >= 20000) {
				err = "prebuilt dependency exceeds file limit";
				return false;
			}
			files.push_back(entry.path);
		}
	}
	return true;
}
bool dependency_parents(
    const std::string &root, const std::string &path, std::string &err)
{
	for (size_t slash = path.find('/'); slash != std::string::npos;
	     slash = path.find('/', slash + 1)) {
		if (create_private_directory(
		        root + "/" + path.substr(0, slash), err))
			continue;
		return false;
	}
	return true;
}
#ifndef _WIN32
// Cross-process ownership also coalesces requests from multiple fibers. Never
// block the event loop in flock: wait cooperatively, with a deadline/cancel.

bool copy_dependency_artifacts(const std::string &source_root,
    const std::string &base, const std::vector<std::string> &artifacts,
    const std::string &target, draft_io_budget_t &budget,
    const std::function<bool()> &cancelled, std::string &err)
{
	agent_workspace_t source(source_root), destination(target);
	for (const auto &artifact : artifacts) {
		if (cancelled && cancelled()) {
			err = "source dependency build cancelled";
			return false;
		}
		const std::string path =
		    base.empty() ? artifact : base + "/" + artifact;
		std::string absolute;
		if (!agent_workspace_t::resolve_project_root(
		        source_root, path, absolute, err)) {
			err = "dependency artifact unavailable: " + path +
			    ": " + err;
			return false;
		}
		struct stat st;
		if (lstat(absolute.c_str(), &st) != 0)
			return false;
		std::vector<std::string> files, directories;
		if (S_ISDIR(st.st_mode)) {
			directories.push_back(path);
			if (!dependency_files(source, path, files, 0, budget,
			        err, directories))
				return false;
		} else if (S_ISREG(st.st_mode))
			files.push_back(path);
		else {
			err =
			    "dependency artifact is not a regular file or directory: " +
			    path;
			return false;
		}
		const auto relative = [&](const std::string &value) {
			return base.empty() ? value :
			                      value.substr(base.size() + 1);
		};
		for (const auto &directory : directories) {
			budget.checkpoint();
			if (cancelled && cancelled()) {
				err = "source dependency build cancelled";
				return false;
			}
			if (dependency_parents(target,
			        relative(directory) + "/placeholder", err))
				continue;
			return false;
		}
		for (const auto &file : files) {
			budget.checkpoint();
			if (cancelled && cancelled()) {
				err = "source dependency build cancelled";
				return false;
			}
			if (!(!dependency_parents(
			          target, relative(file), err) ||
			        !source.copy_build_dependency(
			            file, destination, relative(file), err)))
				continue;
			return false;
		}
	}
	return true;
}
// Installed shared libraries often export file aliases (libfoo.so ->
// libfoo.so.1). Flatten only aliases that resolve to regular files inside the
// install prefix. The cache never inherits an escaping or directory symlink.

std::string dependency_tool_identity(const std::string &path)
{
	char canonical[4096];
	struct stat st;
	if (!realpath(path.c_str(), canonical) || stat(canonical, &st) != 0)
		return path + ":missing";
#ifdef __APPLE__
	const auto mt = st.st_mtimespec, ct = st.st_ctimespec;
#else
	const auto mt = st.st_mtim, ct = st.st_ctim;
#endif
	return std::string(canonical) + ":" + std::to_string(st.st_ino) + ":" +
	    std::to_string(st.st_size) + ":" + std::to_string(mt.tv_sec) + ":" +
	    std::to_string(mt.tv_nsec) + ":" + std::to_string(ct.tv_sec) + ":" +
	    std::to_string(ct.tv_nsec);
}
// Cache identity includes the declared build inputs and tool identity so a
// different configuration cannot silently reuse incompatible artifacts.
static std::string source_dependency_identity(const std::string &system,
    const std::string &platform, const std::string &source_digest,
    const prebuilt_dependency_t &dep, const sandbox_command_t &configure,
    const sandbox_command_t &build)
{
	std::string identity = "source-" + system + "-v1\n" + platform + "\n" +
	    source_digest + "\n" + dep.build_type + "\n";
	if (system == "make") {
		identity += dep.make_target + "\n" + dep.install_target + "\n" +
		    dep.prefix_variable + "\n";
		for (const auto &arg : build.fixed_arguments) {
			identity += arg + "\n";
			const size_t equal = arg.find('=');
			if (!(equal != std::string::npos))
				continue;
			const std::string value = arg.substr(equal + 1);
			const size_t sdk = value.find(" -isysroot ");
			identity +=
			    dependency_tool_identity(value.substr(0, sdk)) +
			    "\n";
			if (!(sdk != std::string::npos))
				continue;
			identity +=
			    dependency_tool_identity(value.substr(sdk + 11)) +
			    "\n";
		}
	}
	auto artifacts = dep.artifacts;
	std::sort(artifacts.begin(), artifacts.end());
	for (const auto &item : artifacts)
		identity += "artifact:" + item + "\n";
	for (const auto &item : dep.definitions)
		identity += "option:" + item.first + "=" + item.second + "\n";
	identity += dependency_tool_identity(configure.executable) + "\n" +
	    dependency_tool_identity(build.executable) + "\n";
	for (const auto &argument : configure.fixed_arguments) {
		identity += argument + "\n";
		const size_t equal = argument.find('=');
		if (!(equal != std::string::npos &&
		        equal + 1 < argument.size() &&
		        argument[equal + 1] == '/'))
			continue;
		identity +=
		    dependency_tool_identity(argument.substr(equal + 1)) + "\n";
	}
	for (const auto *path :
	    { "/usr/bin/cc", "/usr/bin/c++", "/usr/bin/make", "/usr/bin/ld" })
		identity += dependency_tool_identity(path) + "\n";
	identity +=
	    configure.allow_outbound_network ? "network:on" : "network:off";
	return identity;
}

static bool build_source_dependency(const std::string &system,
    const prebuilt_dependency_t &dep, dependency_temporary_t &work,
    sandbox_command_t &configure, sandbox_command_t &build,
    draft_io_budget_t &budget, agent_workspace_t &records,
    const std::string &key, const std::function<bool()> &cancelled,
    std::string &err)
{
	std::vector<sandbox_command_t> commands;
	std::vector<const char *> phases;
	if (system == "cmake") {
		configure.fixed_arguments.push_back(
		    "-DCMAKE_BUILD_TYPE=" + dep.build_type);
		configure.fixed_arguments.push_back("-DCMAKE_INSTALL_PREFIX=" +
		    work.path + "/.webcool-build/install");
		for (const auto &option : dep.definitions)
			configure.fixed_arguments.push_back(
			    "-D" + option.first + "=" + option.second);
		build.fixed_arguments.insert(build.fixed_arguments.end(),
		    { "--config", dep.build_type });
		sandbox_command_t install = build;
		install.id = "dependency.cmake.install";
		install.fixed_arguments = { "--install", ".webcool-build",
			"--config", dep.build_type };
		commands = { configure, build, install };
		phases = { "dependency_configure", "dependency_build",
			"dependency_install" };
	} else {
		build.fixed_arguments.push_back(dep.prefix_variable + "=" +
		    work.path + "/.webcool-build/install");
		sandbox_command_t install = build;
		build.fixed_arguments.push_back(dep.make_target);
		install.id = "dependency.make.install";
		install.fixed_arguments.push_back(dep.install_target);
		commands = { build, install };
		phases = { "dependency_build", "dependency_install" };
	}
	sandbox_limits_t limits;
	limits.timeout_ms = 300000;
	limits.cpu_seconds = 240;
	limits.memory_bytes = 2ULL * 1024 * 1024 * 1024;
	limits.open_files = 512;
	limits.file_size_bytes = 512ULL * 1024 * 1024;
	limits.output_bytes = 128 * 1024;
	for (size_t i = 0; i < commands.size(); ++i) {
		if (cancelled && cancelled()) {
			err = "source dependency build cancelled";
			return false;
		}
		sandbox_request_t request;
		request.command_id = commands[i].id;
		sandbox_result_t result;
		bool ok;
		{
			draft_phase_t phase(budget.run, phases[i]);
			ok = phase.ok =
			    program_sandbox_t(
			        work.path, "", { commands[i] }, limits)
			        .execute(request, result, NULL, cancelled) &&
			    result.exit_code == 0 && !result.cancelled &&
			    !result.timed_out;
		}
		logger(
		    "AI component=agent.draft event=dependency_build_stage run_id=%s root=%s phase=%s ok=%d elapsed_ms=%llu exit_code=%d",
		    budget.run.c_str(), dep.root.c_str(), phases[i], ok,
		    result.elapsed_ms, result.exit_code);
		if (ok)
			continue;
		std::string diagnostic = result.error + "\n" +
		    result.standard_output + "\n" + result.standard_error;
		if (diagnostic.size() > 8000)
			diagnostic =
			    diagnostic.substr(diagnostic.size() - 8000);
		err = "source dependency " + dep.root + " failed at " +
		    phases[i] + " (exit=" + std::to_string(result.exit_code) +
		    "): " + diagnostic;
		if (result.cancelled)
			return false;
		std::string ignored;
		records.save_generated_text(key + ".failed", err, ignored);

		return false;
	}
	return true;
}

static void configure_dependency_make(
    const std::string &system, sandbox_command_t &build)
{
#ifdef __APPLE__
	if (system == "make") {
		const std::string xcode =
		    "/Applications/Xcode.app/Contents/Developer";
		const std::string clt = "/Library/Developer/CommandLineTools";
		const bool full =
		    access(
		        (xcode +
		            "/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang++")
		            .c_str(),
		        X_OK) == 0;
		const std::string developer = full ? xcode : clt;
		const std::string bin = developer +
		    (full ? "/Toolchains/XcodeDefault.xctoolchain/usr/bin/" :
		            "/usr/bin/");
		if (build.executable == "/usr/bin/make" &&
		    access((developer + "/usr/bin/make").c_str(), X_OK) == 0)
			build.executable = developer + "/usr/bin/make";
		for (const auto &tool : { std::make_pair("CC=", "clang"),
		         std::make_pair("CXX=", "clang++"),
		         std::make_pair("AR=", "ar"),
		         std::make_pair("RANLIB=", "ranlib") }) {
			if (!(access((bin + tool.second).c_str(), X_OK) == 0))
				continue;

			std::string value =
			    std::string(tool.first) + bin + tool.second;
			if (std::string(tool.first) == "CC=" ||
			    std::string(tool.first) == "CXX=")
				value += " -isysroot " + developer +
				    (full ? "/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk" :
				            "/SDKs/MacOSX.sdk");
			build.fixed_arguments.push_back(value);
		}
	}
#endif
}

bool prepare_source_dependency(const std::string &user_root,
    const std::string &base, const std::string &metadata,
    const prebuilt_dependency_t &dep, const std::string &platform,
    std::string &cache, draft_io_budget_t &budget,
    const std::function<bool()> &cancelled, std::string &err)
{
	if (cancelled && cancelled()) {
		err = "source dependency build cancelled";
		return false;
	}
	agent_workspace_t source(user_root);
	std::vector<workspace_entry_t> entries;
	std::vector<std::string> source_paths;
	if (!source.list(base, entries, err))
		return false;
	if (entries.size() >= 2000) {
		err = "source dependency has too many root entries";
		return false;
	}
	bool has_cmake = false, has_make = false;
	for (const auto &entry : entries) {
		const std::string relative = entry.path.substr(base.size() + 1);
		if (relative != ".webcool-build")
			source_paths.push_back(relative);
		if (relative == "CMakeLists.txt")
			has_cmake = true;
		if (!(relative == "Makefile" || relative == "makefile" ||
		        relative == "GNUmakefile"))
			continue;
		has_make = true;
	}
	std::string system = dep.build_system;
	if (system == "auto") {
		if (has_cmake == has_make) {
			err = has_cmake ?
			    "dependency has both CMakeLists.txt and Makefile; specify build.system=cmake or make" :
			    "dependency has no CMakeLists.txt or Makefile; configure its supported build files first";
			return false;
		}
		system = has_cmake ? "cmake" : "make";
	}
	if ((system == "make" && dep.has_cmake_options) ||
	    (system == "cmake" && dep.has_make_options)) {
		err =
		    "dependency options do not match detected build system; specify build.system and matching options";
		return false;
	}
	project_toolchain_t toolchain;
	if (!project_toolchain_catalog_t(user_root).discover(
	        base, toolchain, err))
		return false;
	sandbox_command_t configure, build;
	for (const auto &command : toolchain.commands) {
		if (system == "cmake" &&
		    command.id.find(".cmake.configure") != std::string::npos)
			configure = command;
		if (system == "cmake" &&
		    command.id.find(".cmake.build") != std::string::npos)
			build = command;
		if (!(system == "make" &&
		        (command.id == "c.make" || command.id == "cpp.make")))
			continue;
		build = command;
	}
	if (build.executable.empty() ||
	    (system == "cmake" && configure.executable.empty())) {
		err = "source dependency " + dep.root +
		    " requires matching build files and an enabled " + system +
		    " toolchain";
		return false;
	}
	configure_dependency_make(system, build);
	std::string source_digest;
	if (source_paths.empty() ||
	    !source.tree_sha256(base, source_digest, err, source_paths))
		return false;
	const std::string identity = source_dependency_identity(
	    system, platform, source_digest, dep, configure, build);
	const std::string key = agent_workspace_t::content_sha256(identity);
	const std::string packages = metadata + "/dependencies";
	cache = packages + "/" + key;
	dependency_build_lock_t lock;
	if (!lock.acquire(packages + "/" + key + ".lock", cancelled, err))
		return false;
	if (lock.waited) {
		std::string current;
		if (!source.tree_sha256(base, current, err, source_paths) ||
		    current != source_digest) {
			err =
			    "source dependency changed while waiting for its build; retry with current source";
			return false;
		}
	}
	agent_workspace_t records(packages);
	struct stat st;
	if (lstat(cache.c_str(), &st) == 0) {
		std::string expected, actual;
		bool truncated = false;
		if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode) ||
		    !records.read(key + ".built", expected, truncated, err) ||
		    truncated ||
		    !agent_workspace_t(cache).tree_sha256(
		        "", actual, err, dep.artifacts) ||
		    expected != actual) {
			err =
			    "source dependency cache is incomplete or changed: " +
			    dep.root;
			return false;
		}
		logger(
		    "AI component=agent.draft event=source_dependency run_id=%s root=%s cache_hit=1 build_executed=0",
		    budget.run.c_str(), dep.root.c_str());
		return true;
	}
	if (lstat((packages + "/" + key + ".failed").c_str(), &st) == 0 &&
	    time(NULL) - st.st_mtime < 300) {
		std::string failure;
		bool truncated = false;
		if (!records.read(key + ".failed", failure, truncated, err))
			return false;
		err =
		    "source dependency recently failed; identical retry suppressed for 5 minutes (change source/options or retry later): " +
		    failure;
		return false;
	}
	dependency_temporary_t work, package;
	if (!work.create(packages, err) || !package.create(packages, err))
		return false;
	{
		draft_phase_t phase(budget.run, "dependency_copy_source");
		if (!(phase.ok = copy_dependency_artifacts(user_root, base,
		          source_paths, work.path, phase.budget, cancelled,
		          err)))
			return false;
	}
	std::string copied_digest;
	if (!agent_workspace_t(work.path).tree_sha256(
	        "", copied_digest, err, source_paths) ||
	    copied_digest != source_digest) {
		err =
		    "source dependency changed during preparation: " + dep.root;
		return false;
	}
	if (!build_source_dependency(system, dep, work, configure, build,
	        budget, records, key, cancelled, err))
		return false;
	std::string installed;
	// Resolve through the private workspace before treating install as a root;
	// CMake must not trick the publisher into following an escaping symlink.
	if (!agent_workspace_t::resolve_project_root(
	        work.path, ".webcool-build/install", installed, err)) {
		err = "source dependency " + dep.root +
		    " did not produce a safe install directory: " + err;
		std::string ignored;
		records.save_generated_text(key + ".failed", err, ignored);
		return false;
	}
	installed_artifact_copy_t exporter{ installed, package.path, budget,
		cancelled };
	for (const auto &artifact : dep.artifacts) {
		if (exporter.copy(artifact, 0, err))
			continue;

		err = "source dependency " + dep.root +
		    " produced missing/invalid installed artifacts: " + err;
		if (!(!cancelled || !cancelled()))
			return false;
		std::string ignored;
		records.save_generated_text(key + ".failed", err, ignored);

		return false;
	}
	std::string artifact_digest;
	if (!agent_workspace_t(package.path)
	         .tree_sha256("", artifact_digest, err, dep.artifacts))
		return false;
	std::string current_source;
	if (!source.tree_sha256(base, current_source, err, source_paths) ||
	    current_source != source_digest) {
		err =
		    "source dependency changed during build; retry with current source";
		return false;
	}
	if (!records.save_generated_text(
	        key + ".built", artifact_digest, err) ||
	    rename(package.path.c_str(), cache.c_str()) != 0) {
		if (!err.empty())
			return false;
		err = "cannot publish source dependency cache";
		return false;
	}
	logger(
	    "AI component=agent.draft event=source_dependency run_id=%s root=%s cache_hit=0 build_executed=1",
	    budget.run.c_str(), dep.root.c_str());
	return true;
}
#endif

#ifndef _WIN32
// Resolve every declared artifact through workspace containment checks before
// traversing it; the resulting list drives the bounded private-cache copy.
static bool collect_prebuilt_artifacts(agent_workspace_t &source,
    const std::string &user_root, const std::string &base,
    const prebuilt_dependency_t &dep, std::vector<std::string> &files,
    std::vector<std::string> &directories, draft_io_budget_t &budget,
    std::string &err)
{
	struct stat st;
	for (const auto &artifact : dep.artifacts) {
		const std::string path = base + "/" + artifact;
		std::string absolute;
		if (!agent_workspace_t::resolve_project_root(
		        user_root, path, absolute, err))
			return false;
		if (lstat(absolute.c_str(), &st) != 0)
			return false;
		if (S_ISDIR(st.st_mode)) {
			directories.push_back(path);
			if (dependency_files(source, path, files, 0, budget,
			        err, directories))
				continue;
			return false;
		} else
			files.push_back(path);
	}

	return true;
}

static bool copy_prebuilt_artifacts(const std::string &next,
    const std::string &base, const std::vector<std::string> &directories,
    const std::vector<std::string> &files, agent_workspace_t &source,
    agent_workspace_t &target, draft_io_budget_t &budget, std::string &err)
{
	bool ok = true;
	for (const auto &directory : directories) {
		budget.checkpoint();
		if (dependency_parents(next,
		        directory.substr(base.size() + 1) + "/placeholder",
		        err))
			continue;
		ok = false;
		break;
	}
	for (const auto &file : files) {
		if (!ok)
			break;
		budget.checkpoint();
		const std::string relative = file.substr(base.size() + 1);
		if (!(!dependency_parents(next, relative, err) ||
		        !source.copy_build_dependency(
		            file, target, relative, err)))
			continue;
		ok = false;
		break;
	}

	return ok;
}

#endif

bool prepare_dependencies(const std::string &user_root,
    const std::string &project, const std::string &metadata,
    const std::vector<prebuilt_dependency_t> &deps,
    std::map<std::string, std::string> &mounts, draft_io_budget_t &budget,
    std::string &err, const std::function<bool()> &should_cancel)
{
	mounts.clear();
	if (deps.empty())
		return true;
#ifdef _WIN32
	err =
	    "prebuilt dependency reuse requires a read-only dependency sandbox on this platform";
	return false;
#else
	if (!create_private_directory(metadata, err) ||
	    !create_private_directory(metadata + "/dependencies", err))
		return false;
	agent_workspace_t source(user_root);
	struct utsname platform;
	if (uname(&platform) != 0) {
		err = "cannot identify prebuilt dependency platform";
		return false;
	}
	for (const auto &dep : deps) {
		const std::string base =
		    project.empty() ? dep.root : project + "/" + dep.root;
		if (!dep.build_system.empty()) {
			std::string cache;
			if (!prepare_source_dependency(user_root, base,
			        metadata, dep,
			        std::string(platform.sysname) + "/" +
			            platform.machine + "/" + platform.release,
			        cache, budget, should_cancel, err))
				return false;
			mounts[dep.root] = cache;
			continue;
		}
		std::vector<std::string> files, directories;
		for (const auto &artifact : dep.artifacts) {
			const std::string path = base + "/" + artifact;
			std::string absolute;
			if (!agent_workspace_t::resolve_project_root(
			        user_root, path, absolute, err)) {
				err = "prebuilt artifact unavailable: " + path +
				    ": " + err;
				return false;
			}
			struct stat st;
			if (lstat(absolute.c_str(), &st) != 0) {
				err = "missing prebuilt artifact: " + path;
				return false;
			}
			if (!(!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)))
				continue;
			err =
			    "prebuilt artifact must be a regular file or directory: " +
			    path;
			return false;
		}
		std::string digest;
		if (!source.tree_sha256(base, digest, err, dep.artifacts))
			return false;
		std::string declaration;
		for (const auto &artifact : dep.artifacts)
			declaration += artifact + "\n";
		const std::string key = agent_workspace_t::content_sha256(
		    "prebuilt-v1\n" + std::string(platform.sysname) + "\n" +
		    platform.machine + "\n" + declaration + digest);
		const std::string cache = metadata + "/dependencies/" + key;
		struct stat st;
		bool hit = lstat(cache.c_str(), &st) == 0;
		if (hit && (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))) {
			err = "unsafe prebuilt dependency cache directory";
			return false;
		}
		if (!hit) {
			if (!collect_prebuilt_artifacts(source, user_root, base,
			        dep, files, directories, budget, err))
				return false;
			std::sort(files.begin(), files.end());
			std::string pattern =
			    metadata + "/dependencies/.prepare-XXXXXX";
			std::vector<char> temporary(
			    pattern.begin(), pattern.end());
			temporary.push_back(0);
			char *created = mkdtemp(temporary.data());
			if (!created) {
				err =
				    "cannot create dependency cache staging directory";
				return false;
			}
			const std::string next = created;
			agent_workspace_t target(next);
			bool ok = copy_prebuilt_artifacts(next, base,
			    directories, files, source, target, budget, err);
			std::string copied_digest;
			if (ok)
				ok = target.tree_sha256("", copied_digest, err,
				         dep.artifacts) &&
				    copied_digest == digest;
			if (ok && rename(next.c_str(), cache.c_str()) != 0)
				ok = lstat(cache.c_str(), &st) == 0 &&
				    S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);
			std::string cleanup_error;
			remove_tree(next, cleanup_error);
			if (!ok) {
				err = err.empty() ?
				    "prebuilt dependency changed while preparing cache" :
				    err;
				return false;
			}
		}
		// Verify cached content before granting read access. Content hashes use
		// the bounded inode/ctime cache, so unchanged packages are not reread.
		agent_workspace_t cached(cache);
		std::string actual;
		if (!cached.tree_sha256("", actual, err, dep.artifacts) ||
		    actual != digest) {
			err =
			    "prebuilt dependency cache changed; remove the damaged cache before retrying: " +
			    key;
			return false;
		}
		mounts[dep.root] = cache;
		logger(
		    "AI component=agent.draft event=prebuilt_dependency root=%s cache_hit=%d artifact_paths=%zu copied_files=%zu",
		    dep.root.c_str(), hit, dep.artifacts.size(),
		    hit ? 0 : files.size());
	}
	return true;
#endif
}
bool dependency_links(
    const std::string &root, const std::map<std::string, std::string> &mounts)
{
#ifndef _WIN32
	for (const auto &mount : mounts) {
		char target[4096];
		const ssize_t size = readlink(
		    (root + "/" + mount.first).c_str(), target, sizeof(target));
		if (!(size < 0 ||
		        std::string(target, static_cast<size_t>(size)) !=
		            mount.second))
			continue;
		return false;
	}
#endif
	return true;
}
}
using namespace draft_store_detail;
}
}
