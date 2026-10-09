#pragma once

#ifdef _WIN32
#include "platform_compat.h"

namespace webcool
{
namespace ai
{
namespace windows_browser_runtime
{

// Return the directory containing the current executable.
inline std::wstring application_directory()
{
	std::vector<wchar_t> path(32768, L'\0');
	const DWORD length = GetModuleFileNameW(
	    NULL, path.data(), static_cast<DWORD>(path.size()));
	if (!length || length >= path.size())
		return L"";
	const std::wstring executable(path.data(), length);
	const size_t slash = executable.find_last_of(L"\\/");
	return slash == std::wstring::npos ? L"" : executable.substr(0, slash);
}

// Check whether the configured file can be opened for reading.
inline bool readable_file(const std::wstring &path)
{
	const DWORD attributes = GetFileAttributesW(path.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES ||
	    (attributes & FILE_ATTRIBUTE_DIRECTORY))
		return false;
	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
	    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	    OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return false;
	LARGE_INTEGER size;
	const bool valid = GetFileSizeEx(file, &size) && size.QuadPart > 0;
	CloseHandle(file);
	return valid;
}

// Expand directory wildcards one component at a time: Win32 FindFirstFile
// only supports wildcards in the last component, unlike POSIX glob().
inline bool matching_file(const std::wstring &pattern)
{
	const size_t wildcard = pattern.find_first_of(L"*?");
	if (wildcard == std::wstring::npos)
		return readable_file(pattern);
	const size_t slash = pattern.find_first_of(L"\\/", wildcard);
	if (slash == std::wstring::npos)
		return false;
	const std::wstring directory_pattern = pattern.substr(0, slash);
	const size_t parent = directory_pattern.find_last_of(L"\\/");
	WIN32_FIND_DATAW entry;
	HANDLE search = FindFirstFileW(directory_pattern.c_str(), &entry);
	if (search == INVALID_HANDLE_VALUE)
		return false;
	bool found = false;
	do {
		if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
		    !wcscmp(entry.cFileName, L".") ||
		    !wcscmp(entry.cFileName, L".."))
			continue;
		if (!matching_file(directory_pattern.substr(0, parent + 1) +
		        entry.cFileName + pattern.substr(slash)))
			continue;
		found = true;
		break;

	} while (FindNextFileW(search, &entry));
	FindClose(search);
	return found;
}

// Check that the bundled browser runtime is usable; explain failure in
// reason.
inline bool available(const std::wstring &directory, std::string &reason)
{
	reason.clear();
	if (directory.empty()) {
		reason = "cannot locate the WebCool installation directory";
		return false;
	}
	const std::wstring browser = directory + L"/browser";
	for (const wchar_t *relative :
	    { L"/browser_probe.cjs", L"/node_modules/playwright/index.js",
	        L"/node_modules/playwright-core/index.js" }) {
		if (readable_file(browser + relative))
			continue;
		reason =
		    "browser runner or Playwright dependencies are missing";
		return false;
	}
	if (!matching_file(browser +
	        L"/.browsers/chromium_headless_shell-*/chrome-headless-shell-win*/chrome-headless-shell.exe")) {
		reason = "bundled Chromium headless shell is missing";
		return false;
	}
	if (matching_file(
	        browser + L"/.browsers/firefox-*/firefox/firefox.exe"))
		return true;
	reason = "bundled Firefox is missing";
	return false;
}

// Resolve the Node executable bundled with the browser runtime.
inline std::string bundled_node(const std::wstring &directory)
{
	if (directory.empty())
		return "";
	for (const wchar_t *relative :
	    { L"/browser/node.exe", L"/browser/.node/runtime/node.exe" }) {
		const std::wstring path = directory + relative;
		std::string utf8;
		if (!(readable_file(path) &&
		        webcool_wide_to_utf8(path.c_str(), utf8)))
			continue;
		return utf8;
	}
	return "";
}

}
}
}
#endif
