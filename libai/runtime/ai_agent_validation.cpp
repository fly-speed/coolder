#include "stdafx.h"
#include "coding_runtime.h"
#include "../validation/repair_scope.h"
#include "../validation/diagnostic_source.h"
namespace action
{
namespace agent_detail
{
// Inspect CTest's evaluated commands instead of guessing CMake source syntax.
// Paths are metadata only until resolved through the private workspace reader.
struct ctest_io_contract_t {
	std::map<std::string, std::set<std::string>> files;
	std::string error;
};
static void inspect_ctest_arguments(const std::vector<std::string> &args,
    const std::function<void(std::string)> &add_path, const std::string &name,
    ctest_io_contract_t &result)
{
	for (size_t i = 0; i < args.size(); ++i) {
		if (i > 0 && args[i - 1] == "-P")
			add_path(args[i]);
		for (const char *key :
		    { "INPUT_FILE", "EXPECTED_FILE", "EXPECTED_STDERR_FILE" }) {
			const std::string prefix =
			    std::string("-D") + key + "=";
			if (!(args[i].compare(0, prefix.size(), prefix) == 0))
				continue;
			add_path(args[i].substr(prefix.size()));
		}
		for (const char *key : { "APP_ARGS", "EXPECTED_EXIT",
		         "EXPECTED_STDERR_FILE", "STRIP_PROMPTS" }) {
			if (!(args[i] == key))
				continue;
			result.error += "CMake Error: IO test " + name +
			    " passes bare " + key +
			    "; use webcool_add_io_test or -D" + key +
			    "=value before -P.\n";
		}
	}
}

ctest_io_contract_t inspect_ctest_io(
    const std::string &output, const std::string &draft_root)
{
	ctest_io_contract_t result;
	acl::json json(output.c_str());
	acl::json_node *tests =
	    json.finish() ? json_array_node(json["tests"]) : NULL;
	if (!tests) {
		result.error =
		    "Cannot inspect CTest test inventory; validation is incomplete";
		return result;
	}
	for (acl::json_node *item = tests->first_child(); item;
	     item = tests->next_child()) {
		acl::json_node *test =
		    item->is_object() ? item : item->get_obj();
		if (!test)
			continue;
		acl::json_node *command = json_array_node((*test)["command"]);
		std::vector<std::string> args;
		for (acl::json_node *arg = command ? command->first_child() :
		                                     NULL;
		     arg; arg = command->next_child())
			args.push_back(json_text(arg));
		bool checker = false;
		for (size_t i = 1; i < args.size(); ++i) {
			if (!(args[i - 1] == "-P" &&
			        (args[i] == "webcool_io_test.cmake" ||
			            (args[i].size() >= 22 &&
			                args[i].substr(args[i].size() - 22) ==
			                    "/webcool_io_test.cmake"))))
				continue;
			checker = true;
		}
		if (!checker)
			continue;
		const std::string name = json_text((*test)["name"]);
		auto &files = result.files[name];
		files.insert("CMakeLists.txt");
		auto add_path = [&](std::string path) {
			if (path.compare(0, draft_root.size() + 1,
			        draft_root + "/") == 0)
				path.erase(0, draft_root.size() + 1);
			std::string normalized, err;
			if (webcool::ai::agent_workspace_t::normalize_path(
			        path, normalized, false, err))
				files.insert(normalized);
		};
		inspect_ctest_arguments(args, add_path, name, result);
		acl::json_node *properties =
		    json_array_node((*test)["properties"]);
		for (acl::json_node *prop =
		         properties ? properties->first_child() : NULL;
		     prop; prop = properties->next_child()) {
			acl::json_node *object =
			    prop->is_object() ? prop : prop->get_obj();
			if (!(object &&
			        json_text((*object)["name"]) == "WILL_FAIL" &&
			        json_bool((*object)["value"], false)))
				continue;
			result.error += "CMake Error: IO test " + name +
			    " uses WILL_FAIL, which can turn checker assertion failures into passes. Remove WILL_FAIL; pass -DEXPECTED_EXIT=1 (or the required application exit code). The checker itself must succeed.\n";
		}
	}
	return result;
}

static long long append_validation_diagnostics(acl::json &json,
    acl::json_node &item, const ctest_io_contract_t &io_contract,
    const webcool::ai::sandbox_result_t &result, const std::string &draft_root,
    std::string diagnostic)
{
	acl::json_node &related = json.create_array();
	item.add_child("failed_test_files", related);
	std::set<std::string> related_paths;
	bool all_failed_tests_mapped = true;
	const std::regex failed_test(
	    R"(Test\s+#\d+:\s+([^\s]+).*\*\*\*(Failed|Timeout|Exception))");
	for (auto match = std::sregex_iterator(
	         diagnostic.begin(), diagnostic.end(), failed_test);
	     match != std::sregex_iterator(); ++match) {
		const auto found = io_contract.files.find((*match)[1].str());
		if (found != io_contract.files.end())
			related_paths.insert(
			    found->second.begin(), found->second.end());
		else
			all_failed_tests_mapped = false;
	}
	if (!io_contract.error.empty())
		for (const auto &test : io_contract.files)
			related_paths.insert(
			    test.second.begin(), test.second.end());
	for (const auto &path : related_paths)
		related.add_array_text(path.c_str());
	item.add_bool("failed_test_files_complete",
	    all_failed_tests_mapped && !related_paths.empty());
	if (diagnostic.size() > 4096)
		diagnostic.resize(4096);
	if (!diagnostic.empty())
		item.add_text("diagnostic", diagnostic.c_str());
	if (!result.error.empty())
		item.add_text("error", result.error.c_str());
	std::vector<webcool::ai::project_diagnostic_t> parsed;
	webcool::ai::parse_project_diagnostics(
	    result.standard_error + "\n" + result.standard_output, draft_root,
	    "", parsed);
	std::vector<std::string> source_files;
	if (std::any_of(parsed.begin(), parsed.end(),
	        [](const webcool::ai::project_diagnostic_t &source_diagnostic) {
		return source_diagnostic.path.find('/') == std::string::npos;
	}))
		source_files = webcool::ai::diagnostic_source_files(
		    webcool::ai::agent_workspace_t(draft_root));
	for (auto &source_diagnostic : parsed) {
		if (!(source_diagnostic.path.find('/') == std::string::npos))
			continue;

		const auto resolved = webcool::ai::unique_diagnostic_source(
		    source_diagnostic.path, source_files);
		if (resolved.empty())
			continue;
		source_diagnostic.path = resolved;
	}
	acl::json_node &diagnostic_items = json.create_array();
	item.add_child("diagnostics", diagnostic_items);
	for (size_t diagnostic_index = 0; diagnostic_index < parsed.size();
	     ++diagnostic_index) {
		acl::json_node &diagnostic_node =
		    diagnostic_items.add_child(false, true);
		diagnostic_node.add_text(
		    "path", parsed[diagnostic_index].path.c_str());
		diagnostic_node.add_number(
		    "line", parsed[diagnostic_index].line);
		diagnostic_node.add_number(
		    "column", parsed[diagnostic_index].column);
		diagnostic_node.add_text(
		    "severity", parsed[diagnostic_index].severity.c_str());
		diagnostic_node.add_text(
		    "message", parsed[diagnostic_index].message.c_str());
	}
	return static_cast<long long>(parsed.size());
}

// Invoke a fixed project file inside the existing sandbox. No model-supplied
// command text or service environment is passed to the interpreter.
static void append_build_arguments(
    const webcool::ai::sandbox_command_t &command,
    const std::function<void(const std::string &)> &add_directory,
    std::vector<std::string> &arguments)
{
	for (const auto &arg : command.fixed_arguments) {
		const std::string sdk_prefix = "-DCMAKE_OSX_SYSROOT=";
		if (arg.compare(0, sdk_prefix.size(), sdk_prefix) == 0)
			arguments.push_back(
			    "SDKROOT=" + arg.substr(sdk_prefix.size()));
		for (const auto &entry :
		    { std::make_pair("-DCMAKE_C_COMPILER=", "CC="),
		        std::make_pair("-DCMAKE_CXX_COMPILER=", "CXX="),
		        std::make_pair("-DCMAKE_MAKE_PROGRAM=", "") }) {
			const std::string prefix(entry.first);
			if (arg.compare(0, prefix.size(), prefix) != 0)
				continue;
			const std::string executable =
			    arg.substr(prefix.size());
			add_directory(executable);
			if (!(*entry.second))
				continue;
			arguments.push_back(
			    std::string(entry.second) + executable);
		}
	}
}

static void prepend_project_build_script(const std::string &draft_root,
    webcool::ai::project_toolchain_t &toolchain,
    std::vector<std::string> &readonly_roots)
{
#ifndef _WIN32
	webcool::ai::agent_workspace_t draft(draft_root);
	bool exists = false, is_directory = false;
	std::string error;
	if (!workspace_entry_state(
	        draft, "build.sh", exists, is_directory, error) ||
	    !exists || is_directory)
		return;
	bool allowed_build = false;
	std::vector<std::string> arguments;
	std::string search_path;
	const auto add_directory = [&](const std::string &executable) {
		if (executable.empty() || executable[0] != '/' ||
		    executable.find(':') != std::string::npos)
			return;
		char resolved[4096];
		if (!realpath(executable.c_str(), resolved))
			return;
		const std::string canonical(resolved);
		const auto slash = canonical.rfind('/');
		if (slash == std::string::npos)
			return;
		const std::string directory = canonical.substr(0, slash);
		search_path += directory + ":";
		std::string root = directory;
		if (root.size() > 4 && root.substr(root.size() - 4) == "/bin")
			root.resize(root.size() - 4);
		// The env launcher is in /usr; explicitly grant the same read-only
		// compiler installation access that direct compiler execution receives.
		if (!root.empty() && root != "/usr" && root != "/bin" &&
		    root != "/sbin" &&
		    draft_root.compare(0, root.size() + 1, root + "/") != 0 &&
		    root.compare(0, draft_root.size() + 1, draft_root + "/") !=
		        0 &&
		    root != draft_root &&
		    std::find(readonly_roots.begin(), readonly_roots.end(),
		        root) == readonly_roots.end())
			readonly_roots.push_back(root);
	};
	for (const auto &command : toolchain.commands) {
		if (webcool::ai::compile_command(command.id))
			allowed_build = true;
		if (!webcool::ai::compile_command(command.id) &&
		    command.id != "cpp.cmake.configure" &&
		    command.id != "c.cmake.configure")
			continue;
		add_directory(command.executable);
		append_build_arguments(command, add_directory, arguments);
	}
	// Do not use a script to bypass an administrator-disabled toolchain.
	if (!allowed_build)
		return;
	arguments.push_back(
	    "PATH=" + search_path + "/usr/bin:/bin:/usr/sbin:/sbin");
	arguments.push_back("/bin/sh");
	arguments.push_back("./build.sh");
	webcool::ai::sandbox_command_t command;
	command.id = "project.script-build";
	command.executable = "/usr/bin/env";
	command.fixed_arguments = arguments;
	command.allow_outbound_network =
	    webcool::ai::ai_runtime_policy_get().allow_build_network;
	toolchain.commands.insert(toolchain.commands.begin(), command);
#else
	(void)draft_root;
	(void)toolchain;
	(void)readonly_roots;
#endif
}

static bool select_compile_repair_command(
    webcool::ai::sandbox_command_t &command,
    const std::vector<std::string> &packages)
{
	if (!(command.id == "go.test" && !packages.empty()))
		return webcool::ai::compile_command(command.id) ||
		    command.id == "cpp.cmake.configure" ||
		    command.id == "c.cmake.configure";
	command.fixed_arguments = { "test", "-count=1", "-timeout=20s" };
	command.fixed_arguments.insert(
	    command.fixed_arguments.end(), packages.begin(), packages.end());
	return true;
}

static std::vector<std::string> validation_go_packages(
    const std::string &project_path,
    const std::vector<agent_change_proposal_t> &changes)
{
	std::vector<std::string> paths;
	for (const auto &change : changes) {
		paths.push_back(change.path);
		if (change.target_path.empty())
			continue;
		paths.push_back(change.target_path);
	}
	return webcool::ai::changed_go_packages(project_path, paths);
}

static bool dispatch_validation_command(
    const webcool::ai::sandbox_command_t &command,
    const std::string &draft_root, const webcool::ai::sandbox_limits_t &limits,
    const std::vector<std::string> &readonly_roots,
    const std::function<bool()> &should_cancel,
    webcool::ai::sandbox_result_t &result, ctest_io_contract_t &io_contract)
{
	webcool::ai::program_sandbox_t sandbox(draft_root, "",
	    std::vector<webcool::ai::sandbox_command_t>(1, command), limits, "",
	    readonly_roots);
	webcool::ai::sandbox_request_t request;
	request.command_id = command.id;
	bool inventory_ok = true;
	if (command.id == "cpp.ctest" || command.id == "c.ctest") {
		auto inventory_command = command;
		inventory_command.fixed_arguments = { "--test-dir",
			".webcool-build", "--show-only=json-v1" };
		webcool::ai::program_sandbox_t inventory_sandbox(draft_root, "",
		    { inventory_command }, limits, "", readonly_roots);
		webcool::ai::sandbox_result_t inventory;
		inventory_ok = inventory_sandbox.execute(
		                   request, inventory, NULL, should_cancel) &&
		    inventory.exit_code == 0 && !inventory.timed_out &&
		    !inventory.output_truncated && inventory.error.empty();
		if (inventory_ok)
			io_contract = inspect_ctest_io(
			    inventory.standard_output, draft_root);
		else
			io_contract.error =
			    "Cannot inspect CTest test inventory; validation is incomplete: " +
			    inventory.error;
	}
	bool dispatched = false;
	if (io_contract.error.empty())
		dispatched =
		    sandbox.execute(request, result, NULL, should_cancel);
	else {
		dispatched = true;
		result.exit_code = 1;
		result.standard_error = io_contract.error;
	}
	return dispatched;
}

static long long append_validation_command(acl::json &json,
    acl::json_node &commands, const webcool::ai::sandbox_command_t &command,
    const webcool::ai::sandbox_result_t &result,
    const ctest_io_contract_t &io_contract, const std::string &draft_root,
    bool test_command, bool no_tests, bool dispatched, bool command_passed)
{
	acl::json_node &item = commands.add_child(false, true);
	if (test_command)
		item.add_text("test_status",
		    no_tests || !dispatched ? "not-run" :
		        command_passed      ? "passed" :
		                              "failed");
	item.add_text("command_id", command.id.c_str());
	item.add_text("executable", command.executable.c_str());
	item.add_text("scope",
	    !command.browser_probe_node.empty() ?
	        "cross-browser-configured-assertions" :
	        command.id == "functional.acceptance" ? "functional" :
	        command.http_probe_port != 0          ? "http-readiness" :
	                                                "build-or-test");
	item.add_bool("passed",
	    dispatched && result.exit_code == 0 && !result.timed_out &&
	        result.error.empty());
	// Keep browser assertions separately from the 4 KiB compiler diagnostic
	// excerpt: noisy server logs must not hide the actual acceptance result.
	if (!command.browser_probe_node.empty()) {
		const std::string marker = "WEBCOOL_BROWSER_REPORT=";
		const size_t begin = result.standard_output.rfind(marker);
		if (begin != std::string::npos) {
			const size_t end =
			    result.standard_output.find('\n', begin);
			const std::string browser_report =
			    result.standard_output.substr(begin,
			        end == std::string::npos ? end : end - begin);
			item.add_text("browser_report",
			    browser_report.substr(0, 32768).c_str());
			item.add_bool("browser_report_truncated",
			    browser_report.size() > 32768);
		}
	}
	item.add_number("exit_code", result.exit_code);
	item.add_bool("timed_out", result.timed_out);
	// CTest writes assertion details to stdout and only a generic error to stderr.
	std::string diagnostic = result.standard_error;
	if (result.standard_output.empty())
		return append_validation_diagnostics(
		    json, item, io_contract, result, draft_root, diagnostic);
	if (!diagnostic.empty())
		diagnostic += "\n";
	diagnostic += result.standard_output;

	return append_validation_diagnostics(
	    json, item, io_contract, result, draft_root, diagnostic);
}

static const char *validation_next_action(bool compile_repair_completed,
    const std::string &browser_status, const std::string &failed_command_reason,
    const std::string &failed_command_id)
{
	return compile_repair_completed ?
	    "Compilation is confirmed. Report remaining test failures without further edits; do not expand this compile repair into project-wide repair." :
	    browser_status == "unavailable" ?
	    "Browser runtime is unavailable. Report pending browser acceptance and ask the administrator to install Node, Chrome and webcool/browser dependencies; do not change project code to bypass verification." :
	    browser_status == "blocked" ?
	    "Browser did not run: the legacy fixed service port is occupied. Make the service read WEBCOOL_HTTP_PORT (default 18080 when unset), bind 127.0.0.1, and create .webcool-http-port-env. Then validate using an isolated port. Do not kill or test the existing server." :
	    browser_status == "failed" ?
	    "Browser validation failed. Inspect WEBCOOL_BROWSER_REPORT results and differences for each browser/viewport, including clipped/covered elements and screenshots. Repair cross-browser compatibility and rerun the entire matrix; never remove a failing browser or weaken an assertion. Repair the observed cause, retain requirement assertions, then validate the updated draft. Do not infer usability from HTTP 200 or compilation." :
	    failed_command_reason.find("Address already in use") !=
	        std::string::npos ?
	    "HTTP readiness is blocked by an occupied loopback port (18080). Report the port conflict; do not rewrite source or repeatedly rebuild. Retry only after the existing listener is stopped by its owner." :
	    failed_command_id.find(".http-smoke") != std::string::npos ?
	    "HTTP readiness failed. Inspect the failed command executable and error. For CMake, set .webcool-http-service to the project-relative built executable path (for example .webcool-build/test85_web), or build the webcool_app target. Do not repeat validation without correcting the entrypoint or the reported failure." :
	    "repair the reported source locations, then run workspace.validate again";
}

static bool prepare_validation_environment(const std::string &user_root,
    const std::string &project_path, const std::string &run_id,
    const std::string &draft_root, webcool::ai::project_toolchain_t &toolchain,
    std::vector<std::string> &readonly_roots, std::string &err)
{
	std::map<std::string, std::string> mounts;
	if (!webcool::ai::agent_draft_store_t(user_root, project_path, run_id)
	         .readonly_dependencies(mounts, err))
		return false;
	for (const auto &mount : mounts)
		readonly_roots.push_back(mount.second);
	webcool::ai::project_toolchain_catalog_t catalog(draft_root);
	if (!catalog.discover("", toolchain, err))
		return false;
	prepend_project_build_script(draft_root, toolchain, readonly_roots);
	std::string backend_reason;
	if (webcool::ai::program_sandbox_t::backend_available(backend_reason))
		return true;
	webcool::ai::ai_log_error(
	    "agent.draft", "sandbox-unavailable", backend_reason);
	err = backend_reason;
	return false;
}

static acl::json_node &initialize_validation_report(acl::json &json,
    acl::json_node &root, const std::string *source_fingerprint,
    const std::vector<agent_change_proposal_t> &changes, bool build_only,
    bool focused, bool draft_reused, size_t skipped_files)
{
	root.add_text("source_baseline_sha256", source_fingerprint->c_str());
	root.add_text("proposal_sha256",
	    webcool::ai::agent_workspace_t::content_sha256(
	        staged_change_fingerprint(changes))
	        .c_str());
	root.add_text("environment_scope", "private_draft_sandbox");
	root.add_bool("build_network_enabled",
	    webcool::ai::ai_runtime_policy_get().allow_build_network);
	root.add_text("validation_scope",
	    build_only  ? "build-only" :
	        focused ? "compile-repair" :
	                  "project");
	acl::json_node &skipped = json.create_array();
	root.add_child("out_of_scope_commands", skipped);
	root.add_bool("ok", true);
	root.add_bool("draft_reused", draft_reused);
	root.add_number("staged_files", static_cast<long long>(changes.size()));
	root.add_number("skipped_binary_or_large_files",
	    static_cast<long long>(skipped_files));
	return skipped;
}

std::string validate_staged_draft(const std::string &user_root,
    const std::string &project_path, const std::string &run_id,
    const std::vector<agent_change_proposal_t> &changes,
    const webcool::ai::sandbox_limits_t &limits, agent_tool_trace_t &trace,
    std::string *source_fingerprint, bool build_only)
{
	const auto should_cancel = [user_root, run_id] {
		const auto task = find_runtime_task(user_root, run_id);
		return task && runtime_cancel_requested(task);
	};

	const std::string draft_root =
	    persistent_draft_root(user_root, project_path, run_id);
	std::string err;
	size_t skipped_files = 0;
	bool draft_reused = false;
	std::string validated_source;
	if (!source_fingerprint)
		source_fingerprint = &validated_source;
	if (!webcool::ai::agent_draft_store_t(user_root, project_path, run_id)
	         .materialize(changes, skipped_files, err, &draft_reused,
	             source_fingerprint, [user_root, run_id] {
		const auto task = find_runtime_task(user_root, run_id);
		return task && runtime_cancel_requested(task);
	}))
		return tool_error_json(err);

	webcool::ai::project_toolchain_t toolchain;
	std::vector<std::string> readonly_roots;
	if (!prepare_validation_environment(user_root, project_path, run_id,
	        draft_root, toolchain, readonly_roots, err))
		return tool_error_json(err);
	const auto task = find_runtime_task(user_root, run_id);
	const bool focused =
	    task && webcool::ai::focused_compile_repair(task->original_prompt);
	const auto packages = validation_go_packages(project_path, changes);
	// Finish every build before running any advisory tests in mixed-language projects.
	if (focused || build_only)
		std::stable_partition(toolchain.commands.begin(),
		    toolchain.commands.end(),
		    [](const webcool::ai::sandbox_command_t &command) {
			return !webcool::ai::is_test_command(command.id);
		});
	bool build_executed = false, build_passed = true;
	acl::json json;
	acl::json_node &root = json.create_node();
	acl::json_node &skipped =
	    initialize_validation_report(json, root, source_fingerprint,
	        changes, build_only, focused, draft_reused, skipped_files);
	acl::json_node &commands = json.create_array();
	root.add_child("commands", commands);
	bool passed = true;
	size_t executed = 0;
	bool http_passed = false;
	std::string browser_status =
	    toolchain.browser_acceptance_enabled ? "not-run" : "disabled";
	bool acceptance_passed = false;
	bool acceptance_executed = false;
	bool tests_executed = false;
	bool tests_passed = true;
	std::string failed_command_id, failed_command_reason;
	long long diagnostic_count = 0;
	for (size_t i = 0; i < toolchain.commands.size(); ++i) {
		if (should_cancel())
			return tool_error_json("agent run cancelled");
		webcool::ai::sandbox_command_t command = toolchain.commands[i];
		if (!command.browser_probe_node.empty() &&
		    !webcool::ai::agent_workspace_t::resolve_project_root(
		        user_root, project_path, command.browser_evidence_root,
		        err))
			return tool_error_json(err);
		if (command.id.compare(0, 4, "git.") == 0)
			continue;
		if (build_only && !toolchain.commands.empty() &&
		    toolchain.commands.front().id == "project.script-build" &&
		    command.id != "project.script-build")
			continue;
		if ((focused || build_only) &&
		    (!select_compile_repair_command(command,
		        build_only ? std::vector<std::string>() : packages))) {
			skipped.add_array_text(command.id.c_str());
			continue;
		}
		webcool::ai::sandbox_result_t result;
		ctest_io_contract_t io_contract;
		const bool dispatched =
		    dispatch_validation_command(command, draft_root, limits,
		        readonly_roots, should_cancel, result, io_contract);

		const bool command_passed = dispatched &&
		    result.exit_code == 0 && !result.timed_out &&
		    result.error.empty();
		if (webcool::ai::compile_command(command.id)) {
			build_executed = true;
			build_passed = build_passed && command_passed;
		}
		if (command.http_probe_port != 0)
			http_passed = command_passed;
		if (!command.browser_probe_node.empty())
			browser_status = command_passed ? "passed" :
			    result.exit_code == 78      ? "unavailable" :
			    result.exit_code == 73      ? "blocked" :
			                                  "failed";
		if (command.id == "functional.acceptance") {
			acceptance_executed = true;
			acceptance_passed = command_passed;
		}
		// `go build ./src` does not compile test-only files. A test build failure
		// must remain a compiler blocker rather than an advisory assertion failure.
		if (focused && command.id == "go.test" &&
		    (result.standard_output.find("[build failed]") !=
		            std::string::npos ||
		        result.standard_error.find("[build failed]") !=
		            std::string::npos))
			build_passed = false;
		const bool test_command =
		    webcool::ai::is_test_command(command.id);
		const bool no_tests = test_command && command_passed &&
		    !(command.id == "go.test" &&
		        result.standard_output.find("ok ") !=
		            std::string::npos) &&
		    webcool::ai::reports_no_tests(
		        result.standard_error + "\n" + result.standard_output);
		if (test_command && dispatched && !no_tests) {
			tests_executed = true;
			tests_passed = tests_passed && command_passed;
		}
		diagnostic_count += append_validation_command(json, commands,
		    command, result, io_contract, draft_root, test_command,
		    no_tests, dispatched, command_passed);
		++executed;
		if (result.cancelled || should_cancel())
			return tool_error_json("agent run cancelled");
		if (!(!dispatched || result.exit_code != 0 ||
		        result.timed_out || !result.error.empty()))
			continue;
		passed = false;
		failed_command_id = command.id;
		failed_command_reason = result.error + "\n" +
		    result.standard_error + "\n" + result.standard_output;
		break;
	}
	// A focused task may finish with test warnings; never label failing tests as passed.
	const bool compile_repair_completed = focused && build_executed &&
	    build_passed &&
	    (passed || webcool::ai::is_test_command(failed_command_id));
	root.add_bool("compile_repair_completed", compile_repair_completed);
	root.add_bool("validation_passed", passed && executed > 0);
	root.add_text("test_status",
	    tests_executed ? (tests_passed ? "passed" : "failed") : "not-run");
	root.add_bool("http_readiness_passed", http_passed);
	root.add_text("browser_acceptance", browser_status.c_str());
	root.add_bool(
	    "browser_acceptance_enabled", toolchain.browser_acceptance_enabled);
	root.add_text("browser_unavailable_reason",
	    toolchain.browser_unavailable_reason.c_str());
	root.add_text(
	    "browser_acceptance_contract", "tests/browser.acceptance.json");
	root.add_text("functional_acceptance",
	    acceptance_executed ? (acceptance_passed ? "passed" : "failed") :
	                          "not-verified");
	root.add_text("acceptance_contract", "tests/acceptance.test.cjs");
	root.add_text("validation_note",
	    "Build/test and HTTP readiness do not prove "
	    "business functionality. Functional evidence covers only the assertions "
	    "in the configured acceptance test; missing or skipped tests are not verified.");
	root.add_number("commands_executed", static_cast<long long>(executed));
	root.add_number("diagnostic_count", diagnostic_count);
	acl::json_node &unavailable = json.create_array();
	root.add_child("unavailable_tools", unavailable);
	for (size_t i = 0; i < toolchain.unavailable_tools.size(); ++i) {
		unavailable.add_array_text(
		    toolchain.unavailable_tools[i].c_str());
	}
	if (!failed_command_id.empty()) {
		root.add_text("failed_command_id", failed_command_id.c_str());
		root.add_text("next_action",
		    validation_next_action(compile_repair_completed,
		        browser_status, failed_command_reason,
		        failed_command_id));
	}
	if (executed == 0) {
		root.add_bool("validation_available", false);
		root.add_text("message",
		    "no fixed build or test command is available; do not retry validation "
		    "until the draft changes or the administrator enables the reported toolchain");
	} else {
		root.add_bool("validation_available", true);
	}
	trace.ok = true;
	return serialize_json(root);
}

} // namespace agent_detail
} // namespace action
