#include "stdafx.h"
#include "../common/identifiers.h"
// Attachments, provider selection, initial context and completion summaries.
#include "coding_runtime.h"

namespace action
{
namespace agent_detail
{

bool local_project_location(const std::string &input, std::string &logical,
			    std::string &physical, std::string &err)
{
#ifdef _WIN32
	const bool absolute =
		(input.size() >= 3 && input[1] == ':' &&
		 (input[2] == '/' || input[2] == '\\')) ||
		(input.size() >= 2 && input[0] == '\\' && input[1] == '\\');
#else
	const bool absolute = !input.empty() && input[0] == '/';
#endif
	if (!absolute) {
		err = "local project path must be an absolute directory path";
		return false;
	}
	std::string normalized;
	if (!webcool::ai::agent_workspace_t::normalize_path(input, normalized,
							    false, err))
		return false;
	logical = std::string("本地磁盘/") + normalized;
	char resolved[PATH_MAX];
	if (realpath(input.c_str(), resolved) == NULL) {
		err = "local project directory does not exist";
		return false;
	}
	physical = resolved;
	return true;
}

bool safe_git_project_directory(const std::string &physical)
{
	const std::string marker = physical + "/.git";
#ifdef _WIN32
	std::wstring wide;
	if (!webcool_utf8_path_to_wide(marker.c_str(), wide))
		return false;
	const DWORD attributes = GetFileAttributesW(wide.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES &&
	       (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
#else
	struct stat st;
	return lstat(marker.c_str(), &st) == 0 && !S_ISLNK(st.st_mode) &&
	       (S_ISDIR(st.st_mode) || S_ISREG(st.st_mode));
#endif
}

const size_t kMaxAttachmentCount = 8;
const size_t kMaxAttachmentBytes = 8 * 1024 * 1024;
const size_t kMaxAttachmentTotalBytes = 16 * 1024 * 1024;
const size_t kMaxAttachmentTextBytes = 32 * 1024;
const char *kAttachmentDraftPrefix = ".webcool_agent/attachments/draft-";

bool valid_attachment_draft(const std::string &draft)
{
	const std::string prefix(kAttachmentDraftPrefix);
	if (draft.size() != prefix.size() + 24 ||
	    draft.compare(0, prefix.size(), prefix) != 0)
		return false;
	for (size_t i = prefix.size(); i < draft.size(); ++i) {
		const char ch = draft[i];
		if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
			return false;
	}
	return true;
}

std::string attachment_absolute_path(const std::string &user_root,
				     const std::string &relative)
{
	if (user_root.empty())
		return relative;
	const char last = user_root[user_root.size() - 1];
	return last == '/' || last == '\\' ? user_root + relative :
					     user_root + "/" + relative;
}

std::string attachment_mime_type(const std::string &name)
{
	std::string lower = name;
	std::transform(lower.begin(), lower.end(), lower.begin(), [](char ch) {
		return static_cast<char>(
			std::tolower(static_cast<unsigned char>(ch)));
	});
	if (lower.size() >= 4 &&
	    lower.compare(lower.size() - 4, 4, ".png") == 0)
		return "image/png";
	if ((lower.size() >= 4 &&
	     lower.compare(lower.size() - 4, 4, ".jpg") == 0) ||
	    (lower.size() >= 5 &&
	     lower.compare(lower.size() - 5, 5, ".jpeg") == 0))
		return "image/jpeg";
	if (lower.size() >= 5 &&
	    lower.compare(lower.size() - 5, 5, ".webp") == 0)
		return "image/webp";
	if (lower.size() >= 4 &&
	    lower.compare(lower.size() - 4, 4, ".gif") == 0)
		return "image/gif";
	return "";
}

bool load_temporary_attachments(
	acl::json_node *attachment_node, const std::string &draft,
	const std::string &user_root,
	std::vector<webcool::ai::completion_image_t> &images,
	std::string &text_context, std::string &err)
{
	images.clear();
	text_context.clear();
	std::vector<std::string> paths;
	if (!parse_string_array(attachment_node, kMaxAttachmentCount, paths)) {
		err = "attachments must be a bounded string array";
		return false;
	}
	if (paths.empty())
		return draft.empty() || valid_attachment_draft(draft);
	if (!valid_attachment_draft(draft)) {
		err = "invalid temporary attachment draft";
		return false;
	}
	const std::string path_prefix = draft + "/";
	size_t total_bytes = 0;
	for (size_t i = 0; i < paths.size(); ++i) {
		const std::string &relative = paths[i];
		if (relative.compare(0, path_prefix.size(), path_prefix) != 0 ||
		    relative.find('/', path_prefix.size()) !=
			    std::string::npos) {
			err = "attachment path is outside its temporary draft";
			return false;
		}
		const std::string absolute =
			attachment_absolute_path(user_root, relative);
		struct stat st;
		if (lstat(absolute.c_str(), &st) != 0 || !S_ISREG(st.st_mode)
#ifdef S_ISLNK
		    || S_ISLNK(st.st_mode)
#endif
		    || st.st_size <= 0 ||
		    static_cast<unsigned long long>(st.st_size) >
			    kMaxAttachmentBytes) {
			err = "temporary attachment is missing or exceeds 8 MiB";
			return false;
		}
		total_bytes += static_cast<size_t>(st.st_size);
		if (total_bytes > kMaxAttachmentTotalBytes) {
			err = "temporary attachments exceed 16 MiB";
			return false;
		}
		std::ifstream input(absolute.c_str(),
				    std::ios::in | std::ios::binary);
		std::string data(static_cast<size_t>(st.st_size), '\0');
		if (!input || !input.read(&data[0], st.st_size)) {
			err = "cannot read temporary attachment";
			return false;
		}
		const std::string name = relative.substr(path_prefix.size());
		const std::string mime = attachment_mime_type(name);
		if (!mime.empty()) {
			webcool::ai::completion_image_t image;
			image.name = name;
			image.mime_type = mime;
			image.data.swap(data);
			images.push_back(image);
		} else {
			if (data.find('\0') != std::string::npos ||
			    text_context.size() + data.size() >
				    kMaxAttachmentTextBytes) {
				err = "non-image attachments must be text and total at most 32 KiB";
				return false;
			}
			text_context += "\n\n<request_attachment name=\"" +
					name + "\">\n";
			text_context += data;
			text_context += "\n</request_attachment>";
		}
	}
	return true;
}

void remove_temporary_attachment_draft(const std::string &draft,
				       const std::string &user_root)
{
	if (!valid_attachment_draft(draft))
		return;
	const std::string absolute = attachment_absolute_path(user_root, draft);
	DIR *directory = opendir(absolute.c_str());
	if (directory == NULL)
		return;
	for (dirent *entry = readdir(directory); entry != NULL;
	     entry = readdir(directory)) {
		const std::string name = entry->d_name;
		if (name == "." || name == ".." ||
		    name.find('/') != std::string::npos)
			continue;
		const std::string child =
			attachment_absolute_path(absolute, name);
		struct stat st;
		if (lstat(child.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
			if (unlink(child.c_str()) != 0) {
				webcool::ai::ai_log_error(
					"agent.attachment", "remove-file",
					"cannot remove a temporary attachment file");
			}
		}
	}
	closedir(directory);
	if (rmdir(absolute.c_str()) != 0) {
		webcool::ai::ai_log_error(
			"agent.attachment", "remove-directory",
			"cannot remove a temporary attachment draft");
	}
}

std::string new_run_id()
{
	return ::webcool::ai::identifiers::new_id();
}

const webcool::ai::provider_config_t *
select_provider(const std::vector<webcool::ai::provider_config_t> &providers,
		const std::string &requested)
{
	if (!requested.empty()) {
		for (size_t i = 0; i < providers.size(); ++i) {
			if (providers[i].id == requested &&
			    providers[i].enabled) {
				return &providers[i];
			}
		}
		return NULL;
	}
	for (size_t i = 0; i < providers.size(); ++i) {
		if (providers[i].enabled &&
		    providers[i].protocol != "openai_images" &&
		    providers[i].is_default)
			return &providers[i];
	}
	for (size_t i = 0; i < providers.size(); ++i) {
		if (providers[i].enabled &&
		    providers[i].protocol != "openai_images")
			return &providers[i];
	}
	return NULL;
}

bool provider_identity_contains(const webcool::ai::provider_config_t &provider,
				const std::string &needle)
{
	std::string identity =
		provider.name + " " + provider.model + " " + provider.base_url;
	std::transform(identity.begin(), identity.end(), identity.begin(),
		       [](char ch) {
			       return static_cast<char>(std::tolower(
				       static_cast<unsigned char>(ch)));
		       });
	return identity.find(needle) != std::string::npos;
}

bool provider_is_deepseek(const webcool::ai::provider_config_t &provider)
{
	return provider_identity_contains(provider, "deepseek");
}

bool provider_is_kimi(const webcool::ai::provider_config_t &provider)
{
	return provider_identity_contains(provider, "kimi") ||
	       provider_identity_contains(provider, "moonshot");
}

bool provider_supports_thinking_disable(
	const webcool::ai::provider_config_t &provider)
{
	if (provider_is_deepseek(provider))
		return true;
	if (!provider_is_kimi(provider))
		return false;
	std::string model = provider.model;
	std::transform(model.begin(), model.end(), model.begin(), [](char ch) {
		return static_cast<char>(
			std::tolower(static_cast<unsigned char>(ch)));
	});
	// Kimi documents the switch for K2.5 and K2.6. K2.7 Code and K3 are
	// reasoning-only; silently sending type=disabled would either be ignored or
	// rejected and would make the browser option misleading.
	return model.compare(0, 9, "kimi-k2.5") == 0 ||
	       model.compare(0, 9, "kimi-k2.6") == 0;
}

bool provider_requires_extended_reasoning_budget(
	const webcool::ai::provider_config_t &provider)
{
	if (!provider_is_kimi(provider))
		return false;
	std::string model = provider.model;
	std::transform(model.begin(), model.end(), model.begin(), [](char ch) {
		return static_cast<char>(
			std::tolower(static_cast<unsigned char>(ch)));
	});
	// These coding families do not expose K2.5/K2.6's thinking off switch. An
	// 8K quick-mode budget is routinely consumed before the first tool call, so
	// use a larger per-turn floor when the administrator policy permits it.
	return model.compare(0, std::string("kimi-k2.7-code").size(),
			     "kimi-k2.7-code") == 0 ||
	       model.compare(0, 7, "kimi-k3") == 0;
}

std::string join_relative(const std::string &dir, const std::string &name)
{
	return dir.empty() ? name : dir + "/" + name;
}

void append_project_context(webcool::ai::agent_workspace_t &workspace,
			    const std::string &project_path,
			    bool allow_file_content, std::string &context,
			    std::string &err, bool chinese)
{
	std::vector<webcool::ai::workspace_entry_t> entries;
	if (!workspace.list(project_path, entries, err))
		return;
	std::ostringstream out;
	out << prompt_text(prompt_id::project_directory, chinese)
	    << (project_path.empty() ? "." : project_path)
	    << prompt_text(prompt_id::directory_overview, chinese);
	const size_t count = std::min<size_t>(entries.size(), 500);
	size_t emitted = 0;
	for (size_t i = 0; i < count; ++i) {
		out << (entries[i].directory ?
				prompt_text(prompt_id::directory_entry,
					    chinese) :
				prompt_text(prompt_id::file_entry, chinese))
		    << entries[i].path;
		if (!entries[i].directory)
			out << " (" << entries[i].size << " bytes)";
		out << '\n';
		++emitted;
		if (out.tellp() > 24 * 1024)
			break;
	}
	if (entries.size() > emitted)
		out << prompt_text(prompt_id::directory_truncated, chinese);
	context = out.str();
	if (!allow_file_content) {
		context += prompt_text(prompt_id::provider_content_disabled,
				       chinese);
		return;
	}

	const char *candidates[] = { "AGENTS.md", "README.md",
				     "README",	  "CMakeLists.txt",
				     "Makefile",  "Cargo.toml",
				     "go.mod",	  "package.json" };
	for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]);
	     ++i) {
		if (context.size() >= 56 * 1024)
			break;
		const std::string path =
			join_relative(project_path, candidates[i]);
		// These names are optional context, not failed user read requests.
		const auto entry = std::find_if(
			entries.begin(), entries.end(),
			[&path](const webcool::ai::workspace_entry_t &item) {
				return item.path == path && !item.directory;
			});
		if (entry == entries.end())
			continue;
		std::string content;
		std::string ignored;
		bool truncated = false;
		if (!workspace.read(path, content, truncated, ignored))
			continue;
		const size_t room = 56 * 1024 - context.size();
		if (content.size() > room)
			content.resize(room);
		context += "\n--- " + path + " ---\n" + content;
		if (truncated)
			context +=
				prompt_text(prompt_id::file_truncated, chinese);
	}
}

void append_project_plan_context(
	webcool::ai::agent_workspace_t &workspace,
	const webcool::ai::agent_project_record_t *project,
	std::string &context, bool chinese)
{
	if (project == NULL)
		return;

	std::ostringstream out;
	out << "\n<webcool_project_plan version=\"" << project->plan_version
	    << "\">\n"
	    << prompt_text(prompt_id::project_plan_state, chinese)
	    << prompt_text(prompt_id::project_plan_scope, chinese)
	    << prompt_text(prompt_id::project_plan_acceptance, chinese)
	    << prompt_text(prompt_id::project_goal, chinese) << project->goal
	    << prompt_text(prompt_id::project_modules, chinese);
	for (size_t i = 0; i < project->modules.size(); ++i) {
		const webcool::ai::agent_project_module_t &module =
			project->modules[i];
		out << "- " << module.id << " | " << module.name
		    << " | layer=" << module.layer << " | path=" << module.path;
		// Plans name intended paths, not existing directories. Bound these local
		// probes; only an explicit missing-path result is evidence of absence.
		std::string disk_state = "unknown";
		if (i < 32 && !module.path.empty()) {
			std::vector<webcool::ai::workspace_entry_t> children;
			std::string probe_error;
			if (workspace.list(module.path, children, probe_error,
					   false))
				disk_state = "directory";
			else if (probe_error == "workspace path does not exist")
				disk_state = "missing";
		}
		out << " | disk_state=" << disk_state;
		if (!module.dependencies.empty()) {
			out << " | depends=";
			for (size_t j = 0; j < module.dependencies.size();
			     ++j) {
				if (j)
					out << ',';
				out << module.dependencies[j];
			}
		}
		out << '\n';
		if (out.tellp() > 12 * 1024)
			break;
	}
	out << prompt_text(prompt_id::project_tasks, chinese);
	for (size_t i = 0; i < project->tasks.size(); ++i) {
		const webcool::ai::agent_project_task_t &task =
			project->tasks[i];
		out << "- " << task.id << " | status=" << task.status
		    << " | module=" << task.module_id << " | " << task.title;
		if (webcool::ai::agent_project_store_t::task_ready(*project,
								   task)) {
			out << " | ready=true";
		}
		out << '\n';
		if (task.status == "in_progress") {
			for (size_t j = 0; j < task.acceptance_criteria.size();
			     ++j) {
				out << "  acceptance: "
				    << task.acceptance_criteria[j] << '\n';
			}
			for (size_t j = 0; j < task.test_plan.size(); ++j) {
				out << "  test: " << task.test_plan[j] << '\n';
			}
		}
		if (out.tellp() > 20 * 1024) {
			out << prompt_text(prompt_id::project_plan_truncated,
					   chinese);
			break;
		}
	}
	out << "</webcool_project_plan>\n";
	context += out.str();
}

void append_project_memory_context(const std::string &user_root,
				   const std::string &project_path,
				   const std::string &current_session_id,
				   std::string &context, bool chinese)
{
	// Session memory answers “what happened in this conversation”; project
	// memory answers “what durable decisions were made elsewhere in this same
	// project”. Build the latter from already bounded, user-private summaries so
	// no source code or tool output is copied into a second database.
	webcool::ai::agent_session_store_t store(user_root);
	std::vector<webcool::ai::agent_session_record_t> sessions;
	std::string err;
	if (!store.list_for_project(project_path, current_session_id, 6,
				    sessions, err)) {
		webcool::ai::ai_log_error("agent.runtime",
					  "load-project-memory", err);
		return;
	}
	std::ostringstream out;
	out << "\n<project_memory untrusted=\"true\">\n"
	    << prompt_text(prompt_id::project_memory, chinese);
	size_t included = 0;
	for (size_t i = 0; i < sessions.size() && included < 6; ++i) {
		std::string summary = sessions[i].summary;
		if (summary.size() > 2048)
			summary.resize(2048);
		out << "- " << sessions[i].title
		    << " (updated=" << sessions[i].updated_at << ")\n"
		    << summary << "\n";
		++included;
		if (out.tellp() > 12 * 1024)
			break;
	}
	out << "</project_memory>\n";
	if (included > 0)
		context += out.str();
}

void append_project_index_context(
	const std::string &user_root,
	const webcool::ai::agent_project_record_t *project,
	std::string &context, bool chinese)
{
	if (project == NULL)
		return;
	webcool::ai::agent_project_index_store_t index_store(user_root);
	webcool::ai::agent_project_index_snapshot_t snapshot;
	std::string err;
	// A navigation hint, never evidence that a file still exists or is unchanged.
	// Explicit semantic queries refresh; initial submission must not walk a
	// potentially large dependency tree just to emit a bounded index summary.
	if (!index_store.load(*project, snapshot, err)) {
		webcool::ai::ai_log_error("agent.runtime", "load-project-index",
					  err);
		return;
	}
	if (snapshot.revision == 0)
		return;
	context += prompt_text(prompt_id::project_index_cached, chinese);
	context += webcool::ai::agent_project_index_store_t::prompt_summary(
		snapshot, 24 * 1024, chinese);
}

bool compose_initial_prompt(
	webcool::ai::agent_workspace_t &workspace,
	const webcool::ai::provider_config_t &provider,
	const std::string &user_root, const std::string &project_path,
	const std::string &prompt, const std::string &current_session_id,
	const std::string &execution_mode, const std::string &previous_summary,
	std::string &initial_prompt, std::string &err, bool chinese,
	const webcool::ai::agent_project_record_t *selected_project,
	operation_trace_t *trace)
{
	std::vector<webcool::ai::agent_project_record_t> projects;
	if (trace)
		trace->phase("context_project_record");
	if (selected_project == NULL) {
		webcool::ai::agent_project_store_t store(user_root);
		std::string load_err;
		if (!store.list(0, projects, load_err))
			webcool::ai::ai_log_error("agent.runtime",
						  "load-context-project",
						  load_err);
		for (const auto &project : projects) {
			if (project.project_path == project_path) {
				selected_project = &project;
				break;
			}
		}
	}
	if (trace)
		trace->phase("context_project_memory");
	std::string context;
	append_project_memory_context(user_root, project_path,
				      current_session_id, context, chinese);
	if (trace)
		trace->phase("context_project_plan");
	append_project_plan_context(workspace, selected_project, context,
				    chinese);
	if (trace)
		trace->phase("context_saved_index");
	append_project_index_context(user_root, selected_project, context,
				     chinese);
	if (trace)
		trace->phase("context_root_files");
	std::string workspace_context;
	append_project_context(workspace, project_path,
			       provider.allow_file_content, workspace_context,
			       err, chinese);
	if (!err.empty())
		return false;
	context += workspace_context;
	std::string prefix = prompt;
	prefix += "\n<execution_mode>" + execution_mode + ": ";
	if (execution_mode == "quick") {
		prefix += prompt_text(prompt_id::execution_quick, chinese);
	} else if (execution_mode == "large") {
		prefix += prompt_text(prompt_id::execution_large, chinese);
	} else {
		prefix += prompt_text(prompt_id::execution_standard, chinese);
	}
	prefix += "</execution_mode>\n";
	prefix += prompt_text(prompt_id::turn_contract, chinese);
	if (!previous_summary.empty()) {
		prefix += "\n<conversation_summary>\n" + previous_summary +
			  "\n</conversation_summary>\n";
	}
	if (prefix.size() + context.size() > kMaxInitialPromptBytes) {
		const std::string note = prompt_text(
			prompt_id::initial_context_truncated, chinese);
		const size_t available =
			prefix.size() < kMaxInitialPromptBytes ?
				kMaxInitialPromptBytes - prefix.size() :
				0;
		context.resize(
			available > note.size() ? available - note.size() : 0);
		context += note;
	}
	initial_prompt = prefix + context;
	return true;
}

size_t utf8_character_bytes(const std::string &text, size_t offset)
{
	const unsigned char lead = static_cast<unsigned char>(text[offset]);
	if (lead < 0x80)
		return 1;
	if ((lead & 0xe0) == 0xc0)
		return 2;
	if ((lead & 0xf0) == 0xe0)
		return 3;
	if ((lead & 0xf8) == 0xf0)
		return 4;
	return 1;
}

std::string clean_session_title_text(const std::string &source,
				     size_t maximum_characters,
				     size_t *count_out = NULL)
{
	std::string output;
	size_t count = 0;
	bool pending_space = false;
	for (size_t i = 0; i < source.size() && count < maximum_characters;) {
		const unsigned char ch = static_cast<unsigned char>(source[i]);
		if (ch < 0x80 && (ch <= 0x20 || ch == 0x7f)) {
			pending_space = !output.empty();
			++i;
			continue;
		}
		const size_t bytes = utf8_character_bytes(source, i);
		if (i + bytes > source.size())
			break;
		bool valid = true;
		for (size_t j = 1; j < bytes; ++j) {
			if ((static_cast<unsigned char>(source[i + j]) &
			     0xc0) != 0x80) {
				valid = false;
				break;
			}
		}
		if (!valid) {
			++i;
			continue;
		}
		if (pending_space && count < maximum_characters) {
			output.push_back(' ');
			++count;
		}
		pending_space = false;
		if (count >= maximum_characters)
			break;
		output.append(source, i, bytes);
		i += bytes;
		++count;
	}
	while (!output.empty() && output[output.size() - 1] == ' ')
		output.resize(output.size() - 1);
	if (count_out != NULL)
		*count_out = count;
	return output;
}

std::string deterministic_delivery_summary(
	const std::string &user_request,
	const std::vector<agent_change_proposal_t> &changes,
	const std::string &validation_state)
{
	// Tool-first models sometimes keep reasoning after a successful build instead
	// of emitting their final JSON. Preserve useful conversation memory without a
	// further paid model turn; keep it bounded and never copy source contents.
	std::ostringstream out;
	out << "用户请求摘要："
	    << clean_session_title_text(user_request, 80, NULL)
	    << "\n智能体处理摘要：已生成 " << changes.size()
	    << " 项待审查文件修订";
	if (!changes.empty()) {
		out << "（";
		const size_t shown = std::min<size_t>(changes.size(), 8);
		for (size_t i = 0; i < shown; ++i) {
			if (i != 0)
				out << "、";
			out << changes[i].path;
		}
		if (changes.size() > shown)
			out << "等";
		out << "）";
	}
	out << "。\n当前状态：" << validation_state
	    << "，等待用户审查；正式源码尚未自动修改。"
	    << "\n下一步：接受需要写入的修订，或拒绝后提出新的调整要求。";
	return out.str();
}

std::string bounded_completion_summary(const std::string &value)
{
	const size_t maximum_bytes = 4 * 1024;
	if (value.size() <= maximum_bytes)
		return value;
	size_t keep = maximum_bytes;
	while (keep > 0 &&
	       (static_cast<unsigned char>(value[keep]) & 0xc0) == 0x80)
		--keep;
	return value.substr(0, keep);
}

std::string
completion_summary_for_run(const std::string &provided,
			   const std::string &memory_summary,
			   const std::string &final_text,
			   const std::vector<agent_change_proposal_t> &changes,
			   const std::string &original_prompt)
{
	if (!provided.empty())
		return bounded_completion_summary(provided);
	// Ground automatic completion in this request and its actual revision ledger.
	// Patch mechanism labels and session memory are not descriptions of the work.
	std::ostringstream out;
	std::string task =
		webcool::ai::agent_session_task_title(original_prompt);
	if (!task.empty())
		out << "- 本轮需求：" << task;
	const char *generic[] = { "AI 多块原子补丁产生的待审查修改",
				  "AI 精确文本替换产生的待审查修改",
				  "AI 增量生成的待审查文件",
				  "AI 批量生成的待审查文件",
				  "AI 建议删除的待审查文件",
				  "AI 建议创建的待审查目录",
				  "AI 重命名的待审查新文件",
				  "AI 建议移动的待审查文件" };
	size_t shown = 0;
	for (const auto &change : changes) {
		if (shown == 4)
			break;
		out << (out.str().empty() ? "" : "\n") << "- "
		    << (change.operation == "delete" ? "拟删除：" :
			change.operation == "mkdir"  ? "拟创建目录：" :
						       "已生成修订：")
		    << change.path;
		bool useful = !change.reason.empty();
		for (const char *label : generic)
			if (change.reason == label)
				useful = false;
		if (useful)
			out << "；" << change.reason;
		++shown;
	}
	if (changes.size() > shown)
		out << "（另有 " << changes.size() - shown << " 项变更）";
	// Extract only the current validation status, not processing/next-step memory.
	const std::string label = "当前状态：";
	const size_t status = memory_summary.find(label);
	if (status != std::string::npos) {
		const size_t begin = status + label.size();
		const size_t end = memory_summary.find("\n", begin);
		out << (out.str().empty() ? "" : "\n") << "- 验证与交付："
		    << memory_summary.substr(begin, end == std::string::npos ?
							    std::string::npos :
							    end - begin);
	} else if (!final_text.empty()) {
		out << (out.str().empty() ? "" : "\n") << "- 处理结果："
		    << final_text;
	}
	return bounded_completion_summary(out.str());
}

} // namespace agent_detail
} // namespace action
