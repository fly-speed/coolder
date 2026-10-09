#include "stdafx.h"
#include "project_diagnostics.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <regex>
#include <set>
#include <sstream>

namespace webcool
{
namespace ai
{
namespace
{

const size_t kMaxDiagnostics = 200;
const size_t kMaxDiagnosticMessageBytes = 1000;

std::string slash_path(std::string value)
{
	std::replace(value.begin(), value.end(), '\\', '/');
	while (value.size() > 1 && value[value.size() - 1] == '/')
		value.resize(value.size() - 1);
	return value;
}

std::string strip_ansi_and_controls(const std::string &source)
{
	std::string result;
	result.reserve(source.size());
	for (size_t i = 0; i < source.size(); ++i) {
		const unsigned char ch = static_cast<unsigned char>(source[i]);
		if (ch == 0x1b && i + 1 < source.size() &&
		    source[i + 1] == '[') {
			i += 2;
			while (i < source.size()) {
				const unsigned char code =
					static_cast<unsigned char>(source[i]);
				if (code >= 0x40 && code <= 0x7e)
					break;
				++i;
			}
			continue;
		}
		if (ch >= 32 || ch == '\t')
			result.push_back(static_cast<char>(ch));
	}
	return result;
}

long long positive_number(const std::string &value)
{
	if (value.empty())
		return 0;
	char *end = NULL;
	errno = 0;
	const long long parsed = strtoll(value.c_str(), &end, 10);
	return errno == 0 && end != value.c_str() && *end == '\0' &&
			       parsed > 0 ?
		       parsed :
		       0;
}

std::string normalized_severity(std::string value)
{
	for (size_t i = 0; i < value.size(); ++i) {
		value[i] = static_cast<char>(
			std::tolower(static_cast<unsigned char>(value[i])));
	}
	if (value.find("error") != std::string::npos)
		return "error";
	if (value.find("warning") != std::string::npos)
		return "warning";
	return "note";
}

std::string project_relative_path(const std::string &raw,
				  const std::string &absolute_project_root,
				  const std::string &project_path)
{
	std::string path = slash_path(strip_ansi_and_controls(raw));
	while (path.size() >= 2 && path[0] == '.' && path[1] == '/')
		path.erase(0, 2);
	const std::string absolute = slash_path(absolute_project_root);
	const std::string project = slash_path(project_path);
	if (!absolute.empty() && path.size() > absolute.size() &&
	    path.compare(0, absolute.size(), absolute) == 0 &&
	    path[absolute.size()] == '/') {
		path.erase(0, absolute.size() + 1);
	} else if (!project.empty() && path.size() > project.size() &&
		   path.compare(0, project.size(), project) == 0 &&
		   path[project.size()] == '/') {
		path.erase(0, project.size() + 1);
	}
	// Never return a physical path outside the selected project.
	if (path.empty() || path[0] == '/' || path.find("../") == 0 ||
	    path.find("/../") != std::string::npos ||
	    (path.size() >= 2 && path[1] == ':'))
		return "";
	return path.size() <= 2048 ? path : "";
}

bool append_match(const std::smatch &match, bool msvc,
		  const std::string &absolute_project_root,
		  const std::string &project_path, std::set<std::string> &seen,
		  std::vector<project_diagnostic_t> &diagnostics)
{
	project_diagnostic_t item;
	item.path = project_relative_path(match[1].str(), absolute_project_root,
					  project_path);
	item.line = positive_number(match[2].str());
	item.column = positive_number(match[3].str());
	item.severity = normalized_severity(match[4].str());
	item.message = strip_ansi_and_controls(match[5].str());
	if (item.message.size() > kMaxDiagnosticMessageBytes) {
		item.message.resize(kMaxDiagnosticMessageBytes);
	}
	if (item.message.empty())
		return false;
	std::ostringstream key;
	key << item.path << ':' << item.line << ':' << item.column << ':'
	    << item.severity << ':' << item.message;
	if (!seen.insert(key.str()).second)
		return false;
	(void)msvc;
	diagnostics.push_back(item);
	return true;
}

bool append_direct(const std::string &raw_path, const std::string &raw_line,
		   const std::string &raw_column, const std::string &severity,
		   const std::string &message,
		   const std::string &absolute_project_root,
		   const std::string &project_path, std::set<std::string> &seen,
		   std::vector<project_diagnostic_t> &diagnostics)
{
	project_diagnostic_t item;
	item.path = project_relative_path(raw_path, absolute_project_root,
					  project_path);
	item.line = positive_number(raw_line);
	item.column = positive_number(raw_column);
	item.severity = normalized_severity(severity);
	item.message = strip_ansi_and_controls(message);
	if (item.path.empty() || item.line == 0 || item.message.empty())
		return false;
	if (item.message.size() > kMaxDiagnosticMessageBytes) {
		item.message.resize(kMaxDiagnosticMessageBytes);
	}
	std::ostringstream key;
	key << item.path << ':' << item.line << ':' << item.column << ':'
	    << item.severity << ':' << item.message;
	if (!seen.insert(key.str()).second)
		return false;
	diagnostics.push_back(item);
	return true;
}

} // namespace

void parse_project_diagnostics(const std::string &source,
			       const std::string &absolute_project_root,
			       const std::string &project_path,
			       std::vector<project_diagnostic_t> &diagnostics)
{
	diagnostics.clear();
	// Greedy path capture deliberately supports Windows drive letters.
	const std::regex gcc_pattern(
		"^\\s*(.*):([0-9]+):([0-9]+):\\s*(fatal error|error|warning|note):\\s*(.*)$",
		std::regex::icase);
	const std::regex gcc_no_column_pattern(
		"^\\s*(.*):([0-9]+):\\s*(fatal error|error|warning|note):\\s*(.*)$",
		std::regex::icase);
	const std::regex msvc_pattern(
		"^\\s*(.*)\\(([0-9]+)(?:,([0-9]+))?\\)\\s*:\\s*(fatal error|error|warning|note)[^:]*:\\s*(.*)$",
		std::regex::icase);
	// Go and several script runtimes omit an explicit severity but retain a
	// stable source:line[:column]:message form. Limit this fallback to known
	// source extensions so arbitrary program output is not misclassified.
	const std::regex source_location_pattern(
		"^\\s*(.*\\.(?:go|rs|py|js|mjs|cjs|ts|tsx|java|kt|kts|swift|m|mm))"
		":([0-9]+)(?::([0-9]+))?:\\s*(.+)$",
		std::regex::icase);
	const std::regex python_traceback_pattern(
		"^\\s*File \\\"(.*\\.py)\\\", line ([0-9]+).*$",
		std::regex::icase);
	const std::regex rust_location_pattern(
		"^\\s*-->\\s*(.*\\.rs):([0-9]+):([0-9]+)\\s*$",
		std::regex::icase);
	const std::regex javascript_location_pattern(
		"^\\s*(.*\\.(?:js|mjs|cjs|ts|tsx)):([0-9]+)\\s*$",
		std::regex::icase);
	std::istringstream input(source);
	std::set<std::string> seen;
	std::string line;
	while (diagnostics.size() < kMaxDiagnostics &&
	       std::getline(input, line)) {
		std::smatch match;
		if (std::regex_match(line, match, gcc_pattern)) {
			append_match(match, false, absolute_project_root,
				     project_path, seen, diagnostics);
			continue;
		}
		if (std::regex_match(line, match, msvc_pattern)) {
			append_match(match, true, absolute_project_root,
				     project_path, seen, diagnostics);
			continue;
		}
		if (std::regex_match(line, match, gcc_no_column_pattern)) {
			// Normalize the shorter pattern to the five capture groups expected by
			// append_match by constructing the record directly.
			project_diagnostic_t item;
			item.path = project_relative_path(match[1].str(),
							  absolute_project_root,
							  project_path);
			item.line = positive_number(match[2].str());
			item.severity = normalized_severity(match[3].str());
			item.message = strip_ansi_and_controls(match[4].str());
			if (item.message.size() > kMaxDiagnosticMessageBytes) {
				item.message.resize(kMaxDiagnosticMessageBytes);
			}
			std::ostringstream key;
			key << item.path << ':' << item.line
			    << ":0:" << item.severity << ':' << item.message;
			if (!item.message.empty() &&
			    seen.insert(key.str()).second) {
				diagnostics.push_back(item);
			}
			continue;
		}
		if (line.find("-->") == std::string::npos &&
		    std::regex_match(line, match, source_location_pattern)) {
			append_direct(match[1].str(), match[2].str(),
				      match[3].str(),
				      match[4].str().find("warning") !=
						      std::string::npos ?
					      "warning" :
					      "error",
				      match[4].str(), absolute_project_root,
				      project_path, seen, diagnostics);
			continue;
		}
		if (std::regex_match(line, match, python_traceback_pattern)) {
			append_direct(
				match[1].str(), match[2].str(), "", "error",
				"Python traceback points to this source line",
				absolute_project_root, project_path, seen,
				diagnostics);
			continue;
		}
		if (std::regex_match(line, match, rust_location_pattern)) {
			append_direct(
				match[1].str(), match[2].str(), match[3].str(),
				"error",
				"Rust compiler diagnostic points to this source location",
				absolute_project_root, project_path, seen,
				diagnostics);
			continue;
		}
		if (std::regex_match(line, match,
				     javascript_location_pattern)) {
			append_direct(
				match[1].str(), match[2].str(), "", "error",
				"JavaScript runtime diagnostic points to this source line",
				absolute_project_root, project_path, seen,
				diagnostics);
		}
	}
}

} // namespace ai
} // namespace webcool
