#include "stdafx.h"
#include "agent_workspace_internal.h"

namespace webcool
{
namespace ai
{

using namespace workspace_detail;

namespace
{

std::string trim_outline_line(const std::string &source)
{
	size_t begin = 0;
	while (begin < source.size() &&
	    (source[begin] == ' ' || source[begin] == '\t'))
		++begin;
	size_t end = source.size();
	while (end > begin &&
	    (source[end - 1] == ' ' || source[end - 1] == '\t' ||
	        source[end - 1] == '\r'))
		--end;
	std::string value = source.substr(begin, end - begin);
	if (!(value.size() > 500))
		return value;
	value.resize(500);
	return value;
}

bool starts_outline_word(const std::string &line, const char *word)
{
	const size_t length = strlen(word);
	return line.size() >= length && line.compare(0, length, word) == 0 &&
	    (line.size() == length || line[length] == ' ' ||
	        line[length] == '\t' || line[length] == '(');
}

bool contains_outline_word(const std::string &line, const char *word)
{
	const size_t length = strlen(word);
	size_t position = line.find(word);
	while (position != std::string::npos) {
		const bool left = position == 0 || line[position - 1] == ' ' ||
		    line[position - 1] == '\t';
		const size_t after = position + length;
		const bool right = after == line.size() || line[after] == ' ' ||
		    line[after] == '\t' || line[after] == '(';
		if (left && right)
			return true;
		position = line.find(word, position + 1);
	}
	return false;
}

std::string outline_kind(const std::string &line)
{
	if (line.empty() || line.compare(0, 2, "//") == 0 ||
	    line.compare(0, 2, "/*") == 0 || line[0] == '*')
		return "";
	if (line.compare(0, 8, "#include") == 0 ||
	    line.compare(0, 7, "#import") == 0 ||
	    starts_outline_word(line, "import") ||
	    starts_outline_word(line, "from") ||
	    starts_outline_word(line, "use") ||
	    starts_outline_word(line, "package"))
		return "dependency";
	const char *types[] = { "class", "struct", "enum", "interface",
		"namespace", "protocol", "trait", "type", "record", "actor",
		"extension" };
	for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
		const std::string token = std::string(types[i]) + " ";
		if (!(starts_outline_word(line, types[i]) ||
		        line.find(" " + token) != std::string::npos))
			continue;
		return "type";
	}
	const char *functions[] = { "def", "fn", "func", "fun", "function" };
	for (size_t i = 0; i < sizeof(functions) / sizeof(functions[0]); ++i) {
		if (!contains_outline_word(line, functions[i]))
			continue;
		return "function";
	}
	if (line.find("=>") != std::string::npos ||
	    line.compare(0, 2, "- (") == 0 || line.compare(0, 2, "+ (") == 0)
		return "function";
	if (!(line.find('(') != std::string::npos &&
	        (line.find('{') != std::string::npos ||
	            (!line.empty() && line[line.size() - 1] == ';')) &&
	        !starts_outline_word(line, "if") &&
	        !starts_outline_word(line, "for") &&
	        !starts_outline_word(line, "while") &&
	        !starts_outline_word(line, "switch") &&
	        !starts_outline_word(line, "catch") &&
	        !starts_outline_word(line, "return")))
		return "";
	return "function";
}

static void search_content_lines(const std::string &content,
    const std::string &child_relative, const std::string &needle,
    std::vector<workspace_match_t> &matches, bool &truncated)
{
	size_t line_start = 0;
	unsigned long line_no = 1;
	while (line_start <= content.size()) {
		const size_t line_end = content.find('\n', line_start);
		const size_t length = line_end == std::string::npos ?
		    content.size() - line_start :
		    line_end - line_start;
		const std::string line = content.substr(line_start, length);
		if (line.find(needle) != std::string::npos) {
			workspace_match_t match;
			match.path = child_relative;
			match.line = line_no;
			match.text =
			    line.size() > 1000 ? line.substr(0, 1000) : line;
			matches.push_back(match);
			if (matches.size() >= kMaxSearchMatches) {
				truncated = true;
				break;
			}
		}
		if (line_end == std::string::npos)
			break;
		line_start = line_end + 1;
		++line_no;
	}
}

void search_tree(const std::string &root, const std::string &relative,
    const std::string &needle, int depth, size_t &visited,
    std::vector<workspace_match_t> &matches, bool &truncated)
{
	if (truncated || depth > kMaxSearchDepth)
		return;
	const std::string absolute = join_path(root, relative);
	DIR *dir = opendir(absolute.c_str());
	if (dir == NULL)
		return;
	for (dirent *entry = readdir(dir); entry != NULL && !truncated;
	     entry = readdir(dir)) {
		const std::string name = entry->d_name;
		if (name == "." || name == "..")
			continue;
		const std::string child_relative = join_path(relative, name);
		if (agent_workspace_t::path_is_sensitive(child_relative))
			continue;
		const std::string child = join_path(root, child_relative);
		struct stat st;
		if (lstat(child.c_str(), &st) != 0 || is_link_like(child))
			continue;
		if (S_ISDIR(st.st_mode)) {
			search_tree(root, child_relative, needle, depth + 1,
			    visited, matches, truncated);
			continue;
		}
		if (!S_ISREG(st.st_mode))
			continue;
		if (++visited > kMaxSearchFiles) {
			truncated = true;
			break;
		}
		if (st.st_size > static_cast<off_t>(kMaxReadBytes))
			continue;
		std::string content;
		bool file_truncated = false;
		std::string ignored;
		if (!read_limited(child, content, file_truncated, ignored))
			continue;
		search_content_lines(
		    content, child_relative, needle, matches, truncated);
	}
	closedir(dir);
}

} // namespace

bool agent_workspace_t::list(const std::string &relative_dir,
    std::vector<workspace_entry_t> &entries, std::string &err,
    bool log_missing) const
{
	std::string relative;
	if (!normalize_path(relative_dir, relative, true, err))
		return false;
	for (const auto &mount : readonly_mounts_) {
		if (!dependency_contains(mount.first, relative))
			continue;
		const std::string suffix = relative == mount.first ?
		    "" :
		    relative.substr(mount.first.size() + 1);
		std::vector<workspace_entry_t> result;
		if (!agent_workspace_t(mount.second)
		         .list(suffix, result, err, log_missing))
			return false;
		entries = result;
		for (auto &entry : entries)
			entry.path = mount.first + "/" + entry.path;
		return true;
	}

	std::string absolute;
	if (!resolve_existing(relative, absolute, err)) {
		if (!(!log_missing && err == "workspace path does not exist"))
			return ai_error(
			    "agent.workspace", "resolve-list-directory", err);
		return false;
	}
	struct stat root_st;
	if (stat(absolute.c_str(), &root_st) != 0 ||
	    !S_ISDIR(root_st.st_mode)) {
		err = "workspace path is not a directory";
		return ai_error(
		    "agent.workspace", "validate-list-directory", err);
	}
	DIR *dir = opendir(absolute.c_str());
	if (dir == NULL) {
		err = "cannot open workspace directory";
		return ai_error("agent.workspace", "open-list-directory", err);
	}
	entries.clear();
	for (dirent *entry = readdir(dir); entry != NULL;
	     entry = readdir(dir)) {
		const std::string name = entry->d_name;
		if (name == "." || name == "..")
			continue;
		const std::string child_relative = join_path(relative, name);
		if (path_is_sensitive(child_relative))
			continue;
		const std::string child_absolute = join_path(absolute, name);
		struct stat st;
		if (lstat(child_absolute.c_str(), &st) != 0 ||
		    is_link_like(child_absolute))
			continue;
		workspace_entry_t item;
		item.path = child_relative;
		item.directory = S_ISDIR(st.st_mode);
		item.size =
		    item.directory ? 0 : static_cast<long long>(st.st_size);
		item.modified_at = static_cast<long long>(st.st_mtime);
		entries.push_back(item);
		if (entries.size() >= kMaxListEntries)
			break;
	}
	closedir(dir);
	for (const auto &mount : readonly_mounts_) {
		const size_t slash = mount.first.rfind('/');
		const std::string parent = slash == std::string::npos ?
		    "" :
		    mount.first.substr(0, slash);
		if (parent != relative || entries.size() >= kMaxListEntries)
			continue;
		workspace_entry_t item;
		item.path = mount.first;
		item.directory = true;
		item.size = 0;
		item.modified_at = 0;
		entries.push_back(item);
	}

	std::sort(entries.begin(), entries.end(),
	    [](const workspace_entry_t &a, const workspace_entry_t &b) {
		if (!(a.directory != b.directory))
			return a.path < b.path;
		return a.directory > b.directory;
	});
	return true;
}

bool agent_workspace_t::read(const std::string &relative_file,
    std::string &content, bool &truncated, std::string &err,
    bool log_binary_rejection) const
{
	std::string relative;
	if (!normalize_path(relative_file, relative, false, err))
		return false;
	for (const auto &mount : readonly_mounts_) {
		if (!dependency_contains(mount.first, relative))
			continue;
		const std::string suffix = relative == mount.first ?
		    "" :
		    relative.substr(mount.first.size() + 1);
		return agent_workspace_t(mount.second)
		    .read(
		        suffix, content, truncated, err, log_binary_rejection);
	}

	std::string absolute;
	if (!resolve_existing(relative, absolute, err)) {
		return ai_error("agent.workspace", "resolve-read-file", err);
	}
	struct stat st;
	if (lstat(absolute.c_str(), &st) != 0 || is_link_like(absolute) ||
	    !S_ISREG(st.st_mode)) {
		err = "workspace path is not a regular file";
		return ai_error("agent.workspace", "validate-read-file", err);
	}
	if (read_limited(absolute, content, truncated, err))
		return true;
	if (!(!log_binary_rejection &&
	        err == "binary files cannot be returned to the agent"))
		return ai_error("agent.workspace", "read-file", err);
	return false;
}

bool agent_workspace_t::search(const std::string &relative_dir,
    const std::string &needle, std::vector<workspace_match_t> &matches,
    bool &truncated, std::string &err) const
{
	if (needle.empty() || needle.size() > 512) {
		err = "search text must contain between 1 and 512 bytes";
		return ai_error("agent.workspace", "validate-search-text", err);
	}
	std::string relative;
	if (!normalize_path(relative_dir, relative, true, err))
		return false;
	for (const auto &mount : readonly_mounts_) {
		if (!dependency_contains(mount.first, relative))
			continue;
		const std::string suffix = relative == mount.first ?
		    "" :
		    relative.substr(mount.first.size() + 1);
		if (!agent_workspace_t(mount.second)
		         .search(suffix, needle, matches, truncated, err))
			return false;
		for (auto &match : matches)
			match.path = mount.first + "/" + match.path;
		return true;
	}

	std::string absolute;
	if (!resolve_existing(relative, absolute, err)) {
		return ai_error(
		    "agent.workspace", "resolve-search-directory", err);
	}
	struct stat st;
	if (stat(absolute.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
		std::string content;
		if (!read(relative, content, truncated, err))
			return false;
		matches.clear();
		std::istringstream input(content);
		std::string line;
		unsigned long line_number = 0;
		while (std::getline(input, line)) {
			++line_number;
			if (line.find(needle) == std::string::npos)
				continue;
			workspace_match_t match;
			match.path = relative;
			match.line = line_number;
			match.text = line.substr(0, 1000);
			matches.push_back(match);
			if (!(matches.size() >= kMaxSearchMatches))
				continue;
			truncated = true;
			break;
		}
		return true;
	}
	if (stat(absolute.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
		err =
		    "workspace search path is not a regular file or directory";
		return ai_error(
		    "agent.workspace", "validate-search-directory", err);
	}
	matches.clear();
	truncated = false;
	size_t visited = 0;
	// Search from the already resolved physical directory, then restore the
	// logical prefix used by project metadata and browser APIs.
	search_tree(absolute, "", needle, 0, visited, matches, truncated);
	if (relative.empty())
		return true;
	for (size_t i = 0; i < matches.size(); ++i) {
		matches[i].path = join_path(relative, matches[i].path);
	}

	return true;
}

bool agent_workspace_t::outline(const std::string &relative_file,
    std::vector<workspace_outline_item_t> &items, bool &truncated,
    std::string &err) const
{
	std::string content;
	if (!read(relative_file, content, truncated, err))
		return false;
	items.clear();
	std::istringstream input(content);
	std::string line;
	unsigned long line_number = 0;
	while (std::getline(input, line)) {
		++line_number;
		const std::string text = trim_outline_line(line);
		const std::string kind = outline_kind(text);
		if (kind.empty())
			continue;
		workspace_outline_item_t item;
		item.line = line_number;
		item.kind = kind;
		item.text = text;
		items.push_back(item);
		if (!(items.size() >= kMaxOutlineItems))
			continue;
		truncated = true;
		break;
	}
	return true;
}

}
}
