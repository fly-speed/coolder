#include "stdafx.h"
#include "../common/file_ops.h"
#include "../common/identifiers.h"
#include "../common/record_codec.h"
#include "../prompt/prompt_templates.h"
#include "agent_project_index.h"

#include "../workspace/agent_workspace.h"
#include "../common/ai_error_log.h"
#include "../common/webcool_mutex.h"

#ifdef _WIN32
#include "../common/platform_compat.h"
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <sys/stat.h>
#include <vector>

namespace webcool {
namespace ai {
namespace {

webcool::mutex g_project_index_mutex;
const char* kHeader = "WEBCOOL_AGENT_PROJECT_INDEX_V1";
const size_t kMaxFiles = 20000;
const size_t kMaxDirectories = 4096;
const size_t kMaxDepth = 32;
const size_t kMaxSymbolsPerFile = 128;

using ::webcool::ai::file_ops::join_path;

using ::webcool::ai::record_codec::hex_value;

bool valid_project_id(const std::string& id) {
    return ::webcool::ai::identifiers::valid_id(id);
}

using ::webcool::ai::record_codec::hex_encode;

using ::webcool::ai::record_codec::hex_decode;

using ::webcool::ai::record_codec::split_tabs;

bool parse_number(const std::string& text, long long& value) {
    return ::webcool::ai::record_codec::parse_nonnegative_number(text, value);
}

bool safe_text(const std::string& value, size_t limit, bool required) {
	if ((required && value.empty()) || value.size() > limit) return false;
	for (size_t i = 0; i < value.size(); ++i) {
		const unsigned char c = static_cast<unsigned char>(value[i]);
		if (c == 0 || c == '\r' || c == '\n' || c == '\t' || c == 127) {
			return false;
		}
	}
	return true;
}

std::string index_directory(const std::string& user_root) {
	return join_path(user_root, ".webcool_agent/indexes");
}

std::string index_path(const std::string& user_root, const std::string& id) {
	return join_path(index_directory(user_root), id + ".v1");
}

bool ensure_one_directory(const std::string& path, std::string& err) {
#ifdef _WIN32
	std::wstring wide;
	if (!webcool_utf8_path_to_wide(path.c_str(), wide)) {
		err = "cannot validate agent project index directory";
		return false;
	}
	DWORD attributes = GetFileAttributesW(wide.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES) {
		if (_wmkdir(wide.c_str()) != 0) {
			err = "cannot create agent project index directory";
			return false;
		}
		attributes = GetFileAttributesW(wide.c_str());
	}
	if (attributes == INVALID_FILE_ATTRIBUTES
		|| (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0
		|| (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
	{
		err = "agent project index path is not a safe directory";
		return false;
	}
#else
	struct stat st;
	if (lstat(path.c_str(), &st) != 0) {
		if (mkdir(path.c_str(), 0700) != 0 || lstat(path.c_str(), &st) != 0) {
			err = "cannot create agent project index directory";
			return false;
		}
	}
	if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)
		|| chmod(path.c_str(), 0700) != 0)
	{
		err = "agent project index path is not a safe private directory";
		return false;
	}
#endif
	return true;
}

bool ensure_directories(const std::string& user_root, std::string& err) {
	return ensure_one_directory(join_path(user_root, ".webcool_agent"), err)
		&& ensure_one_directory(index_directory(user_root), err);
}

using ::webcool::ai::file_ops::replace_file;

bool valid_entry(const agent_project_index_entry_t& entry) {
	if (!(safe_text(entry.path, 2048, true)
		&& safe_text(entry.module_id, 64, false)
		&& safe_text(entry.kind, 32, true)
		&& safe_text(entry.language, 32, false)
		&& entry.size >= 0 && entry.modified_at >= 0)) return false;
	if (entry.symbols.size() > kMaxSymbolsPerFile) return false;
	for (size_t i = 0; i < entry.symbols.size(); ++i) {
		if (entry.symbols[i].line == 0
			|| !safe_text(entry.symbols[i].kind, 32, true)
			|| !safe_text(entry.symbols[i].text, 500, true)) return false;
	}
	return true;
}

bool save_snapshot(const std::string& user_root,
	const agent_project_index_snapshot_t& snapshot, std::string& err)
{
	if (!valid_project_id(snapshot.project_id)
		|| !safe_text(snapshot.project_path, 2048, true)
		|| snapshot.files.size() > kMaxFiles)
	{
		err = "invalid agent project index snapshot";
		return false;
	}
	if (!ensure_directories(user_root, err)) return false;
	const std::string path = index_path(user_root, snapshot.project_id);
	const std::string temporary = path + ".tmp";
	std::ofstream out(temporary.c_str(), std::ios::out | std::ios::binary
		| std::ios::trunc);
	if (!out.good()) {
		err = "cannot write agent project index";
		return false;
	}
	out << kHeader << '\n' << "H\t" << hex_encode(snapshot.project_id) << '\t'
		<< hex_encode(snapshot.project_path) << '\t' << snapshot.revision << '\t'
		<< snapshot.indexed_at << '\t' << snapshot.directory_count << '\t'
		<< snapshot.skipped_directory_count << '\t'
		<< snapshot.changed_file_count << '\t' << (snapshot.truncated ? 1 : 0)
		<< '\n';
	for (size_t i = 0; i < snapshot.files.size(); ++i) {
		if (!valid_entry(snapshot.files[i])) {
			out.close();
			remove(temporary.c_str());
			err = "invalid agent project index entry";
			return false;
		}
		const agent_project_index_entry_t& entry = snapshot.files[i];
		out << "E\t" << hex_encode(entry.path) << '\t'
			<< hex_encode(entry.module_id) << '\t' << hex_encode(entry.kind)
			<< '\t' << hex_encode(entry.language) << '\t' << entry.size << '\t'
			<< entry.modified_at << '\n';
		for (size_t symbol_index = 0; symbol_index < entry.symbols.size();
			symbol_index++)
		{
			const agent_project_symbol_t& symbol = entry.symbols[symbol_index];
			out << "S\t" << hex_encode(entry.path) << '\t' << symbol.line << '\t'
				<< hex_encode(symbol.kind) << '\t' << hex_encode(symbol.text) << '\n';
		}
	}
	out.close();
	if (!out.good()) {
		remove(temporary.c_str());
		err = "cannot flush agent project index";
		return false;
	}
#ifndef _WIN32
	if (chmod(temporary.c_str(), 0600) != 0) {
		unlink(temporary.c_str());
		err = "cannot protect agent project index";
		return false;
	}
#endif
	if (!replace_file(temporary, path)) {
		remove(temporary.c_str());
		err = std::string("cannot install agent project index: ") + strerror(errno);
		return false;
	}
	return true;
}

bool load_snapshot(const std::string& user_root,
	const agent_project_record_t& project,
	agent_project_index_snapshot_t& snapshot, std::string& err)
{
	snapshot = agent_project_index_snapshot_t();
	snapshot.project_id = project.id;
	snapshot.project_path = project.project_path;
	std::ifstream in(index_path(user_root, project.id).c_str(),
		std::ios::in | std::ios::binary);
	if (!in.good()) return true;
	std::string line;
	if (!std::getline(in, line) || line != kHeader || !std::getline(in, line)) {
		err = "invalid agent project index";
		return false;
	}
	std::vector<std::string> fields;
	split_tabs(line, fields);
	long long truncated = 0;
	if (fields.size() != 9 || fields[0] != "H"
		|| !hex_decode(fields[1], snapshot.project_id)
		|| !hex_decode(fields[2], snapshot.project_path)
		|| !parse_number(fields[3], snapshot.revision)
		|| !parse_number(fields[4], snapshot.indexed_at)
		|| !parse_number(fields[5], snapshot.directory_count)
		|| !parse_number(fields[6], snapshot.skipped_directory_count)
		|| !parse_number(fields[7], snapshot.changed_file_count)
		|| !parse_number(fields[8], truncated)
		|| snapshot.project_id != project.id
		|| snapshot.project_path != project.project_path
		|| truncated > 1)
	{
		err = "invalid agent project index header";
		return false;
	}
	snapshot.truncated = truncated == 1;
	std::map<std::string, size_t> entry_indexes;
	while (std::getline(in, line)) {
		if (line.empty()) continue;
		split_tabs(line, fields);
		if (fields.size() == 5 && fields[0] == "S") {
			std::string path;
			long long line_number = 0;
			agent_project_symbol_t symbol;
			if (!hex_decode(fields[1], path)
				|| !parse_number(fields[2], line_number) || line_number == 0
				|| !hex_decode(fields[3], symbol.kind)
				|| !hex_decode(fields[4], symbol.text)
				|| !safe_text(symbol.kind, 32, true)
				|| !safe_text(symbol.text, 500, true)
				|| entry_indexes.find(path) == entry_indexes.end())
			{
				err = "invalid agent project semantic index entry";
				return false;
			}
			symbol.line = static_cast<unsigned long>(line_number);
			agent_project_index_entry_t& owner =
				snapshot.files[entry_indexes[path]];
			if (owner.symbols.size() >= kMaxSymbolsPerFile) {
				err = "agent project semantic index exceeds file limit";
				return false;
			}
			owner.symbols.push_back(symbol);
			continue;
		}
		agent_project_index_entry_t entry;
		if (fields.size() != 7 || fields[0] != "E"
			|| !hex_decode(fields[1], entry.path)
			|| !hex_decode(fields[2], entry.module_id)
			|| !hex_decode(fields[3], entry.kind)
			|| !hex_decode(fields[4], entry.language)
			|| !parse_number(fields[5], entry.size)
			|| !parse_number(fields[6], entry.modified_at)
			|| !valid_entry(entry) || snapshot.files.size() >= kMaxFiles)
		{
			err = "invalid agent project index entry";
			return false;
		}
		entry_indexes[entry.path] = snapshot.files.size();
		snapshot.files.push_back(entry);
	}
	return true;
}

std::string basename(const std::string& path) {
	const size_t slash = path.rfind('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string lowercase(std::string value) {
	for (size_t i = 0; i < value.size(); ++i) {
		if (value[i] >= 'A' && value[i] <= 'Z') value[i] += 'a' - 'A';
	}
	return value;
}

bool generated_directory(const std::string& path) {
	const std::string name = lowercase(basename(path));
	return name == "build" || name == "target" || name == "node_modules"
		|| name == ".build" || name == ".gradle" || name == "dist"
		|| name == "out" || name == "__pycache__" || name == ".venv"
		|| name == "venv" || name == "coverage";
}

bool path_within(const std::string& parent, const std::string& path) {
	return path == parent || (path.size() > parent.size()
		&& path.compare(0, parent.size(), parent) == 0
		&& path[parent.size()] == '/');
}

std::string module_for_path(const agent_project_record_t& project,
	const std::string& path)
{
	std::string selected;
	size_t selected_length = 0;
	for (size_t i = 0; i < project.modules.size(); ++i) {
		if (path_within(project.modules[i].path, path)
			&& project.modules[i].path.size() > selected_length)
		{
			selected = project.modules[i].id;
			selected_length = project.modules[i].path.size();
		}
	}
	return selected;
}

std::string extension(const std::string& path) {
	const std::string name = lowercase(basename(path));
	const size_t dot = name.rfind('.');
	return dot == std::string::npos ? "" : name.substr(dot);
}

bool agent_diagnostic_file(const std::string& path) {
	const std::string name = lowercase(basename(path));
	// User-visible reasoning and operation logs intentionally live beside the
	// project for diagnosis. They are not project inputs and must not be sent back
	// to the model on later turns, where they would waste context and may create a
	// self-referential read loop.
	return (name.compare(0, 13, "ai-reasoning-") == 0
			&& extension(name) == ".txt")
		|| (name.compare(0, 14, "ai-operations-") == 0
			&& extension(name) == ".jsonl");
}

std::string file_language(const std::string& path) {
	const std::string ext = extension(path);
	if (ext == ".c" || ext == ".h") return "c";
	if (ext == ".cc" || ext == ".cpp" || ext == ".cxx" || ext == ".hpp"
		|| ext == ".hh") return "cpp";
	if (ext == ".m" || ext == ".mm") return "objective-c";
	if (ext == ".swift") return "swift";
	if (ext == ".go") return "go";
	if (ext == ".rs") return "rust";
	if (ext == ".java") return "java";
	if (ext == ".kt" || ext == ".kts") return "kotlin";
	if (ext == ".cs") return "csharp";
	if (ext == ".js" || ext == ".mjs" || ext == ".cjs") return "javascript";
	if (ext == ".ts" || ext == ".tsx") return "typescript";
	if (ext == ".py") return "python";
	if (ext == ".php" || ext == ".php4" || ext == ".php5"
		|| ext == ".phtml") return "php";
	if (ext == ".d" || ext == ".di") return "d";
	if (ext == ".sh") return "shell";
	if (ext == ".bat" || ext == ".cmd" || ext == ".ps1") return "script";
	return "";
}

std::string file_kind(const std::string& path, const std::string& language) {
	const std::string lower = lowercase(path);
	const std::string name = lowercase(basename(path));
	const std::string ext = extension(path);
	if (lower.find("/test/") != std::string::npos
		|| lower.find("/tests/") != std::string::npos
		|| lower.find("_test.") != std::string::npos
		|| lower.find("test_") != std::string::npos)
	{
		return "test";
	}
	if (name == "makefile" || name == "cmakelists.txt" || name == "cargo.toml"
		|| name == "go.mod" || name == "package.swift" || name == "package.json"
		|| name == "pyproject.toml" || ext == ".csproj" || ext == ".sln"
		|| ext == ".gradle")
	{
		return "build";
	}
	if (ext == ".md" || ext == ".rst" || ext == ".txt") return "documentation";
	if (!language.empty()) return "source";
	if (ext == ".json" || ext == ".yaml" || ext == ".yml"
		|| ext == ".toml" || ext == ".xml" || ext == ".ini")
	{
		return "configuration";
	}
	return "other";
}

bool entry_less(const agent_project_index_entry_t& left,
	const agent_project_index_entry_t& right)
{
	return left.path < right.path;
}

std::string fingerprint(const agent_project_index_entry_t& entry) {
	std::ostringstream out;
	out << entry.size << ':' << entry.modified_at << ':' << entry.module_id
		<< ':' << entry.kind << ':' << entry.language;
	return out.str();
}

long long changed_files(const agent_project_index_snapshot_t& before,
	const agent_project_index_snapshot_t& after)
{
	std::map<std::string, std::string> previous;
	for (size_t i = 0; i < before.files.size(); ++i) {
		previous[before.files[i].path] = fingerprint(before.files[i]);
	}
	long long changed = 0;
	for (size_t i = 0; i < after.files.size(); ++i) {
		std::map<std::string, std::string>::iterator found =
			previous.find(after.files[i].path);
		if (found == previous.end() || found->second != fingerprint(after.files[i])) {
			++changed;
		}
		if (found != previous.end()) previous.erase(found);
	}
	return changed + static_cast<long long>(previous.size());
}

} // namespace

agent_project_index_store_t::agent_project_index_store_t(
	const std::string& user_root) : user_root_(user_root) {}

bool agent_project_index_store_t::remove(const std::string& project_id,
	std::string& err) const
{
	if (!valid_project_id(project_id)) {
		err = "invalid agent project index id";
		return ai_error("agent.project-index", "validate-remove", err);
	}
	std::lock_guard<webcool::mutex> guard(g_project_index_mutex);
	const std::string path = index_path(user_root_, project_id);
	if (::remove(path.c_str()) != 0 && errno != ENOENT) {
		err = std::string("cannot remove agent project index: ") + strerror(errno);
		return ai_error("agent.project-index", "remove", err);
	}
	return true;
}

bool agent_project_index_store_t::load(const agent_project_record_t& project,
	agent_project_index_snapshot_t& snapshot, std::string& err) const
{
	if (!valid_project_id(project.id)) {
		err = "invalid agent project index owner";
		return ai_error("agent.project-index", "validate-load", err);
	}
	std::lock_guard<webcool::mutex> guard(g_project_index_mutex);
	if (!load_snapshot(user_root_, project, snapshot, err)) {
		return ai_error("agent.project-index", "load", err);
	}
	return true;
}

bool agent_project_index_store_t::refresh(
	const agent_project_record_t& project, agent_workspace_t& workspace,
	agent_project_index_snapshot_t& snapshot, std::string& err,
	const std::function<void(const char*)>& phase) const
{
	if (!valid_project_id(project.id)) {
		err = "invalid agent project index owner";
		return ai_error("agent.project-index", "validate-refresh", err);
	}
	// Load the prior snapshot once before walking. Unchanged files reuse their
	// semantic records, so a large project refresh reads only changed sources.
	if (phase) phase("index_load_previous");
	agent_project_index_snapshot_t previous;
	{
		std::lock_guard<webcool::mutex> guard(g_project_index_mutex);
		if (!load_snapshot(user_root_, project, previous, err)) {
			return ai_error("agent.project-index", "load-for-incremental-refresh", err);
		}
	}
	if (phase) phase("index_prepare_lookup");
	std::map<std::string, const agent_project_index_entry_t*> previous_files;
	for (size_t i = 0; i < previous.files.size(); ++i) {
		previous_files[previous.files[i].path] = &previous.files[i];
	}

	snapshot = agent_project_index_snapshot_t();
	snapshot.project_id = project.id;
	snapshot.project_path = project.project_path;
	snapshot.indexed_at = static_cast<long long>(time(NULL));
	std::vector<std::pair<std::string, size_t> > pending;
	pending.push_back(std::make_pair(project.project_path, 0));
	if (phase) phase("index_scan_and_outline");
	for (size_t cursor = 0; cursor < pending.size(); ++cursor) {
		if (cursor >= kMaxDirectories) {
			snapshot.truncated = true;
			break;
		}
		std::vector<workspace_entry_t> entries;
		if (!workspace.list(pending[cursor].first, entries, err)) {
			return ai_error("agent.project-index", "list-workspace", err);
		}
		++snapshot.directory_count;
		for (size_t i = 0; i < entries.size(); ++i) {
			if (entries[i].directory) {
				if (generated_directory(entries[i].path)
					|| pending[cursor].second >= kMaxDepth)
				{
					++snapshot.skipped_directory_count;
					if (pending[cursor].second >= kMaxDepth) snapshot.truncated = true;
					continue;
				}
				pending.push_back(std::make_pair(entries[i].path,
					pending[cursor].second + 1));
				continue;
			}
			if (agent_diagnostic_file(entries[i].path)) continue;
			if (snapshot.files.size() >= kMaxFiles) {
				snapshot.truncated = true;
				break;
			}
			agent_project_index_entry_t indexed;
			indexed.path = entries[i].path;
			indexed.module_id = module_for_path(project, indexed.path);
			indexed.language = file_language(indexed.path);
			indexed.kind = file_kind(indexed.path, indexed.language);
			indexed.size = entries[i].size;
			indexed.modified_at = entries[i].modified_at;
			std::map<std::string, const agent_project_index_entry_t*>::const_iterator
				old = previous_files.find(indexed.path);
			if (old != previous_files.end()
				&& fingerprint(*old->second) == fingerprint(indexed))
			{
				indexed.symbols = old->second->symbols;
			} else if (indexed.kind == "source" && indexed.size <= 1024 * 1024) {
				std::vector<workspace_outline_item_t> outline;
				bool outline_truncated = false;
				std::string outline_err;
				if (workspace.outline(indexed.path, outline, outline_truncated,
					outline_err))
				{
					const size_t count = std::min<size_t>(outline.size(),
						kMaxSymbolsPerFile);
					for (size_t symbol_index = 0; symbol_index < count;
						symbol_index++)
					{
						agent_project_symbol_t symbol;
						symbol.line = outline[symbol_index].line;
						symbol.kind = outline[symbol_index].kind;
						symbol.text = outline[symbol_index].text;
						indexed.symbols.push_back(symbol);
					}
					if (outline_truncated || outline.size() > count) {
						snapshot.truncated = true;
					}
				} else {
					// A single unreadable/binary source should not invalidate the
					// metadata index for the rest of a large project.
					ai_log_error("agent.project-index", "outline-changed-file",
						outline_err);
				}
			}
			snapshot.files.push_back(indexed);
		}
		if (snapshot.files.size() >= kMaxFiles) break;
	}
	if (phase) phase("index_sort");
	std::sort(snapshot.files.begin(), snapshot.files.end(), entry_less);
	if (phase) phase("index_load_latest");
	{
		std::lock_guard<webcool::mutex> guard(g_project_index_mutex);
		// Re-read under the write lock so two simultaneous refreshes receive
		// distinct revisions and compare against the latest committed snapshot.
		agent_project_index_snapshot_t latest;
		if (!load_snapshot(user_root_, project, latest, err)) {
			return ai_error("agent.project-index", "load-for-refresh", err);
		}
        if (phase) phase("index_compare");
        snapshot.changed_file_count = changed_files(latest, snapshot);
        bool identical = latest.revision > 0 && snapshot.changed_file_count == 0
            && latest.project_path == snapshot.project_path
            && latest.directory_count == snapshot.directory_count
            && latest.skipped_directory_count == snapshot.skipped_directory_count
            && latest.truncated == snapshot.truncated;
        // Compare semantic records too; a refresh must not discard a new outline
        // merely because the cheap file metadata fingerprint stayed unchanged.
        for (size_t i = 0; identical && i < snapshot.files.size(); ++i) {
            const auto& before = latest.files[i];
            const auto& after = snapshot.files[i];
            identical = before.path == after.path && before.symbols.size() == after.symbols.size();
            for (size_t j = 0; identical && j < after.symbols.size(); ++j)
                identical = before.symbols[j].line == after.symbols[j].line
                    && before.symbols[j].kind == after.symbols[j].kind
                    && before.symbols[j].text == after.symbols[j].text;
        }
        if (identical) {
            snapshot.revision = latest.revision;
            snapshot.indexed_at = latest.indexed_at;
            return true;
        }
        snapshot.revision = latest.revision + 1;
		if (phase) phase("index_save");
		if (!save_snapshot(user_root_, snapshot, err)) {
			return ai_error("agent.project-index", "save-refresh", err);
		}
	}
	return true;
}

std::string agent_project_index_store_t::prompt_summary(
	const agent_project_index_snapshot_t& snapshot, size_t byte_limit, bool chinese)
{
	if (byte_limit < 256) return "";
	std::ostringstream out;
	out << "\n<webcool_project_index revision=\"" << snapshot.revision
		<< "\" files=\"" << snapshot.files.size() << "\" changed=\""
		<< snapshot.changed_file_count << "\">\n"
		<< (prompt_text(prompt_id::project_index, chinese));
	for (size_t i = 0; i < snapshot.files.size(); ++i) {
		const agent_project_index_entry_t& entry = snapshot.files[i];
		out << "- " << entry.path << " | " << entry.kind;
		if (!entry.language.empty()) out << " | " << entry.language;
		if (!entry.module_id.empty()) out << " | module=" << entry.module_id;
		out << " | " << entry.size << " bytes";
		if (!entry.symbols.empty()) out << " | symbols=" << entry.symbols.size();
		out << "\n";
		for (size_t symbol_index = 0; symbol_index < entry.symbols.size()
			&& symbol_index < 12; ++symbol_index)
		{
			out << "  @" << entry.symbols[symbol_index].line << " "
				<< entry.symbols[symbol_index].kind << " "
				<< entry.symbols[symbol_index].text << "\n";
			if (static_cast<size_t>(out.tellp()) + 80 >= byte_limit) break;
		}
		if (static_cast<size_t>(out.tellp()) + 80 >= byte_limit) {
			out << "[project index truncated]\n";
			break;
		}
	}
	out << "</webcool_project_index>\n";
	std::string result = out.str();
	if (result.size() > byte_limit) result.resize(byte_limit);
	return result;
}

} // namespace ai
} // namespace webcool
