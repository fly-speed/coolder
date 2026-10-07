#pragma once

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <string>
#include <vector>

#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif

#ifndef F_OK
#define F_OK 0
#endif
#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef X_OK
#define X_OK 4
#endif

#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFDIR) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFREG) == _S_IFREG)
#endif
#ifndef S_ISLNK
#define S_ISLNK(m) 0
#endif

typedef int mode_t;
typedef int pid_t;
typedef int uid_t;
typedef intptr_t ssize_t;

#define dup2 _dup2
#define getpid _getpid
#define pclose _pclose
#define popen _popen

inline bool webcool_utf8_to_wide(const char* text, std::wstring& out)
{
	out.clear();
	if (text == NULL) {
		errno = EINVAL;
		return false;
	}
	const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1,
		NULL, 0);
	if (n <= 0) {
		errno = EINVAL;
		return false;
	}
	std::vector<wchar_t> buf((size_t) n);
	if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1,
		&buf[0], n) <= 0) {
		errno = EINVAL;
		return false;
	}
	out.assign(&buf[0]);
	return true;
}

inline bool webcool_wide_to_utf8(const wchar_t* text, std::string& out)
{
	out.clear();
	if (text == NULL) {
		errno = EINVAL;
		return false;
	}
	const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0,
		NULL, NULL);
	if (n <= 0) {
		errno = EINVAL;
		return false;
	}
	std::vector<char> buf((size_t) n);
	if (WideCharToMultiByte(CP_UTF8, 0, text, -1, &buf[0], n,
		NULL, NULL) <= 0) {
		errno = EINVAL;
		return false;
	}
	out.assign(&buf[0]);
	return true;
}

inline bool webcool_is_windows_extended_path(const std::wstring& path)
{
	return path.compare(0, 4, L"\\\\?\\") == 0
		|| path.compare(0, 4, L"\\??\\") == 0;
}

inline bool webcool_is_windows_drive_absolute(const std::wstring& path)
{
	return path.size() >= 3
		&& ((path[0] >= L'A' && path[0] <= L'Z')
			|| (path[0] >= L'a' && path[0] <= L'z'))
		&& path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
}

inline bool webcool_utf8_path_to_wide(const char* path, std::wstring& out)
{
	if (!webcool_utf8_to_wide(path, out)) {
		return false;
	}
	for (size_t i = 0; i < out.size(); ++i) {
		if (out[i] == L'/') {
			out[i] = L'\\';
		}
	}
	if (out.empty() || webcool_is_windows_extended_path(out)) {
		return true;
	}

	std::wstring full_path;
	if (webcool_is_windows_drive_absolute(out)
		|| (out.size() >= 2 && out[0] == L'\\' && out[1] == L'\\')) {
		full_path = out;
	} else {
		wchar_t buffer[32768];
		const DWORD n = GetFullPathNameW(out.c_str(),
			(DWORD) (sizeof(buffer) / sizeof(buffer[0])), buffer, NULL);
		if (n == 0 || n >= sizeof(buffer) / sizeof(buffer[0])) {
			errno = ENAMETOOLONG;
			return false;
		}
		full_path.assign(buffer);
		for (size_t i = 0; i < full_path.size(); ++i) {
			if (full_path[i] == L'/') {
				full_path[i] = L'\\';
			}
		}
	}

	if (full_path.size() >= 2 && full_path[0] == L'\\'
		&& full_path[1] == L'\\') {
		out = L"\\\\?\\UNC\\" + full_path.substr(2);
	} else if (webcool_is_windows_drive_absolute(full_path)) {
		out = L"\\\\?\\" + full_path;
	} else {
		out = full_path;
	}
	return true;
}

inline void webcool_strip_extended_path_prefix(std::wstring& path)
{
	const std::wstring unc_prefix = L"\\\\?\\UNC\\";
	const std::wstring drive_prefix = L"\\\\?\\";
	const std::wstring nt_unc_prefix = L"\\??\\UNC\\";
	const std::wstring nt_drive_prefix = L"\\??\\";
	if (path.compare(0, unc_prefix.size(), unc_prefix) == 0) {
		path = L"\\\\" + path.substr(unc_prefix.size());
	} else if (path.compare(0, drive_prefix.size(), drive_prefix) == 0) {
		path = path.substr(drive_prefix.size());
	} else if (path.compare(0, nt_unc_prefix.size(), nt_unc_prefix) == 0) {
		path = L"\\\\" + path.substr(nt_unc_prefix.size());
	} else if (path.compare(0, nt_drive_prefix.size(), nt_drive_prefix) == 0) {
		path = path.substr(nt_drive_prefix.size());
	}
}

inline int webcool_mkdir(const char* path, int)
{
	std::wstring wpath;
	return webcool_utf8_path_to_wide(path, wpath) ? _wmkdir(wpath.c_str()) : -1;
}

inline int webcool_rmdir(const char* path)
{
	std::wstring wpath;
	return webcool_utf8_path_to_wide(path, wpath) ? _wrmdir(wpath.c_str()) : -1;
}

inline int webcool_unlink(const char* path)
{
	std::wstring wpath;
	return webcool_utf8_path_to_wide(path, wpath) ? _wunlink(wpath.c_str()) : -1;
}

inline int webcool_rename(const char* old_path, const char* new_path)
{
	std::wstring wold_path;
	std::wstring wnew_path;
	if (!webcool_utf8_path_to_wide(old_path, wold_path)
		|| !webcool_utf8_path_to_wide(new_path, wnew_path)) {
		return -1;
	}
	if (MoveFileExW(wold_path.c_str(), wnew_path.c_str(),
		  MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
		return 0;
	}
	const DWORD err = GetLastError();
	if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
		errno = ENOENT;
	} else if (err == ERROR_ACCESS_DENIED || err == ERROR_SHARING_VIOLATION) {
		errno = EACCES;
	} else if (err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS) {
		errno = EEXIST;
	} else if (err == ERROR_NOT_SAME_DEVICE) {
		errno = EXDEV;
	} else {
		errno = EINVAL;
	}
	return -1;
}

inline FILE* webcool_fopen(const char* path, const char* mode)
{
	std::wstring wpath;
	std::wstring wmode;
	if (!webcool_utf8_path_to_wide(path, wpath)
		|| !webcool_utf8_to_wide(mode, wmode)) {
		return NULL;
	}
	return _wfopen(wpath.c_str(), wmode.c_str());
}

inline int webcool_stat(const char* path, struct stat* st)
{
	std::wstring wpath;
	if (st == NULL || !webcool_utf8_path_to_wide(path, wpath)) {
		return -1;
	}
#ifdef _USE_32BIT_TIME_T
	static_assert(sizeof(struct stat) == sizeof(struct _stat32),
		"unexpected stat layout");
	if (_wstat32(wpath.c_str(), reinterpret_cast<struct _stat32*>(st)) == 0) {
		return 0;
	}
#else
	static_assert(sizeof(struct stat) == sizeof(struct _stat64i32),
		"unexpected stat layout");
	if (_wstat64i32(wpath.c_str(), reinterpret_cast<struct _stat64i32*>(st)) == 0) {
		return 0;
	}
#endif
	const errno_t saved_errno = errno;
	WIN32_FILE_ATTRIBUTE_DATA data;
	if (!GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &data)) {
		errno = saved_errno;
		return -1;
	}
	memset(st, 0, sizeof(*st));
	st->st_mode = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		? (_S_IFDIR | 0755) : (_S_IFREG | 0644);
	ULARGE_INTEGER size;
	size.HighPart = data.nFileSizeHigh;
	size.LowPart = data.nFileSizeLow;
	st->st_size = size.QuadPart > 0x7fffffffULL
		? (decltype(st->st_size)) 0x7fffffff
		: (decltype(st->st_size)) size.QuadPart;
	const ULONGLONG ticks_per_second = 10000000ULL;
	const ULONGLONG unix_epoch = 11644473600ULL;
	ULARGE_INTEGER write_time;
	write_time.HighPart = data.ftLastWriteTime.dwHighDateTime;
	write_time.LowPart = data.ftLastWriteTime.dwLowDateTime;
	if (write_time.QuadPart / ticks_per_second > unix_epoch) {
		st->st_mtime = (time_t) (write_time.QuadPart / ticks_per_second - unix_epoch);
		st->st_ctime = st->st_mtime;
		st->st_atime = st->st_mtime;
	}
	return 0;
}

inline int webcool_access(const char* path, int mode)
{
	std::wstring wpath;
	return webcool_utf8_path_to_wide(path, wpath) ? _waccess(wpath.c_str(), mode) : -1;
}

inline int webcool_chmod(const char* path, int mode)
{
	std::wstring wpath;
	return webcool_utf8_path_to_wide(path, wpath) ? _wchmod(wpath.c_str(), mode) : -1;
}

inline void webcool_set_errno_from_windows_error(DWORD err)
{
	if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
		errno = ENOENT;
	} else if (err == ERROR_ACCESS_DENIED || err == ERROR_SHARING_VIOLATION) {
		errno = EACCES;
	} else if (err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS) {
		errno = EEXIST;
	} else if (err == ERROR_DISK_FULL || err == ERROR_HANDLE_DISK_FULL) {
		errno = ENOSPC;
	} else if (err == ERROR_NOT_SAME_DEVICE) {
		errno = EXDEV;
	} else {
		errno = EINVAL;
	}
}

inline bool webcool_copy_file(const char* source, const char* dest,
	bool overwrite)
{
	std::wstring wsource;
	std::wstring wdest;
	if (!webcool_utf8_path_to_wide(source, wsource)
		|| !webcool_utf8_path_to_wide(dest, wdest)) {
		return false;
	}
	if (CopyFileW(wsource.c_str(), wdest.c_str(), overwrite ? FALSE : TRUE)) {
		return true;
	}
	webcool_set_errno_from_windows_error(GetLastError());
	return false;
}

inline bool webcool_make_dirs_utf8(const char* path, int mode)
{
	if (path == NULL || *path == '\0') {
		errno = EINVAL;
		return false;
	}

	struct stat st;
	if (webcool_stat(path, &st) == 0) {
		return S_ISDIR(st.st_mode);
	}

	std::string text(path);
	for (size_t i = 0; i < text.size(); ++i) {
		if (text[i] == '\\') {
			text[i] = '/';
		}
	}

	size_t start = 0;
	if (text.size() >= 2 && text[1] == ':') {
		start = 2;
	} else if (text.size() >= 2 && text[0] == '/' && text[1] == '/') {
		size_t server_end = text.find('/', 2);
		if (server_end == std::string::npos) {
			errno = EINVAL;
			return false;
		}
		size_t share_end = text.find('/', server_end + 1);
		start = share_end == std::string::npos ? text.size() : share_end;
	}

	for (size_t pos = start; pos <= text.size(); ++pos) {
		if (pos != text.size() && text[pos] != '/') {
			continue;
		}
		if (pos == 0 || (pos == 2 && text.size() > 1 && text[1] == ':')) {
			continue;
		}
		const std::string part = text.substr(0, pos);
		if (part.empty()) {
			continue;
		}
		if (webcool_stat(part.c_str(), &st) == 0) {
			if (!S_ISDIR(st.st_mode)) {
				errno = ENOTDIR;
				return false;
			}
			continue;
		}
		if (webcool_mkdir(part.c_str(), mode) != 0 && errno != EEXIST) {
			return false;
		}
	}
	return true;
}

#ifndef SIGTERM
#define SIGTERM 15
#endif
#ifndef WIFEXITED
#define WIFEXITED(status) (1)
#endif
#ifndef WEXITSTATUS
#define WEXITSTATUS(status) (status)
#endif

inline int kill(pid_t pid, int)
{
	HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD) pid);
	if (process == NULL) {
		errno = ESRCH;
		return -1;
	}
	const BOOL ok = TerminateProcess(process, 1);
	CloseHandle(process);
	if (!ok) {
		errno = EPERM;
		return -1;
	}
	return 0;
}

inline int setenv(const char* name, const char* value, int overwrite)
{
	if (!overwrite && getenv(name) != NULL) {
		return 0;
	}
	return _putenv_s(name, value ? value : "");
}

inline uid_t getuid()
{
	return 0;
}

inline char* webcool_realpath(const char* path, char* resolved)
{
	if (path == NULL || resolved == NULL) {
		errno = EINVAL;
		return NULL;
	}
	std::wstring wpath;
	if (!webcool_utf8_path_to_wide(path, wpath)) {
		return NULL;
	}
	wchar_t wresolved[32768];
	const DWORD n = GetFullPathNameW(wpath.c_str(),
		(DWORD) (sizeof(wresolved) / sizeof(wresolved[0])), wresolved, NULL);
	if (n == 0 || n >= sizeof(wresolved) / sizeof(wresolved[0])) {
		errno = ENOENT;
		return NULL;
	}
	std::wstring display_path(wresolved);
	webcool_strip_extended_path_prefix(display_path);
	std::string utf8;
	if (!webcool_wide_to_utf8(display_path.c_str(), utf8)
		|| utf8.size() >= PATH_MAX) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	strcpy(resolved, utf8.c_str());
	return resolved;
}
#define realpath webcool_realpath

inline int readlink(const char*, char*, size_t)
{
	errno = ENOSYS;
	return -1;
}

inline int symlink(const char*, const char*)
{
	errno = ENOSYS;
	return -1;
}

struct dirent {
	char d_name[MAX_PATH];
};

struct DIR {
	HANDLE handle;
	WIN32_FIND_DATAW data;
	struct dirent entry;
	bool first;
};

inline std::wstring webcool_dir_pattern(const char* path)
{
	std::string text = path && *path ? path : ".";
	std::wstring pattern;
	if (!webcool_utf8_path_to_wide(text.c_str(), pattern)) {
		return std::wstring();
	}
	const wchar_t tail = pattern.empty() ? L'\0' : pattern[pattern.size() - 1];
	if (tail != L'/' && tail != L'\\') {
		pattern += L"\\";
	}
	pattern += L"*";
	return pattern;
}

inline DIR* opendir(const char* path)
{
	DIR* dir = new DIR;
	dir->first = true;
	const std::wstring pattern = webcool_dir_pattern(path);
	if (pattern.empty()) {
		delete dir;
		return NULL;
	}
	dir->handle = FindFirstFileW(pattern.c_str(), &dir->data);
	if (dir->handle == INVALID_HANDLE_VALUE) {
		const DWORD err = GetLastError();
		if (err == ERROR_ACCESS_DENIED) {
			errno = EACCES;
		} else if (err == ERROR_PATH_NOT_FOUND || err == ERROR_FILE_NOT_FOUND) {
			errno = ENOENT;
		} else {
			errno = EINVAL;
		}
		delete dir;
		return NULL;
	}
	return dir;
}

inline struct dirent* readdir(DIR* dir)
{
	if (dir == NULL) {
		errno = EBADF;
		return NULL;
	}
	if (!dir->first) {
		if (!FindNextFileW(dir->handle, &dir->data)) {
			return NULL;
		}
	} else {
		dir->first = false;
	}
	std::string name;
	if (!webcool_wide_to_utf8(dir->data.cFileName, name)) {
		return NULL;
	}
	strncpy(dir->entry.d_name, name.c_str(), sizeof(dir->entry.d_name) - 1);
	dir->entry.d_name[sizeof(dir->entry.d_name) - 1] = '\0';
	return &dir->entry;
}

inline int closedir(DIR* dir)
{
	if (dir == NULL) {
		errno = EBADF;
		return -1;
	}
	const int rc = FindClose(dir->handle) ? 0 : -1;
	delete dir;
	return rc;
}

static char* webcool_optarg = NULL;
static int webcool_optind = 1;

inline int webcool_getopt(int argc, char* const argv[], const char* optstring)
{
	static const char* next = NULL;
	if (next == NULL || *next == '\0') {
		if (webcool_optind >= argc || argv[webcool_optind][0] != '-'
			|| argv[webcool_optind][1] == '\0') {
			return -1;
		}
		if (strcmp(argv[webcool_optind], "--") == 0) {
			++webcool_optind;
			return -1;
		}
		next = argv[webcool_optind] + 1;
	}
	const char ch = *next++;
	const char* pos = strchr(optstring, ch);
	if (pos == NULL) {
		if (*next == '\0') {
			++webcool_optind;
		}
		return '?';
	}
	if (pos[1] == ':') {
		if (*next != '\0') {
			webcool_optarg = const_cast<char*>(next);
			++webcool_optind;
			next = NULL;
		} else if (webcool_optind + 1 < argc) {
			webcool_optarg = argv[++webcool_optind];
			++webcool_optind;
			next = NULL;
		} else {
			return '?';
		}
	} else {
		webcool_optarg = NULL;
		if (*next == '\0') {
			++webcool_optind;
		}
	}
	return ch;
}

//#define getopt webcool_getopt
//#define optarg webcool_optarg
//#define optind webcool_optind

inline std::string webcool_windows_home_path()
{
	wchar_t path[MAX_PATH];
	memset(path, 0, sizeof(path));
	if (SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, SHGFP_TYPE_CURRENT, path)
		== S_OK && path[0] != L'\0')
	{
		std::string utf8;
		if (webcool_wide_to_utf8(path, utf8) && !utf8.empty()) {
			return utf8;
		}
	}

	const char* home = getenv("USERPROFILE");
	if (home == NULL || *home == '\0') {
		home = getenv("HOME");
	}
	return home && *home ? home : ".";
}

inline bool webcool_shell_open(const std::string& target, std::string& err)
{
	std::wstring wtarget;
	if (!webcool_utf8_to_wide(target.c_str(), wtarget)) {
		err = "invalid UTF-8 path";
		return false;
	}
	HINSTANCE rc = ShellExecuteW(NULL, L"open", wtarget.c_str(), NULL, NULL, SW_SHOWNORMAL);
	if ((INT_PTR) rc <= 32) {
		err = "ShellExecute failed";
		return false;
	}
	return true;
}

inline bool webcool_shell_open_trash(std::string& err)
{
	return webcool_shell_open("shell:RecycleBinFolder", err);
}

#define access(path, mode) webcool_access(path, mode)
#define chmod(path, mode) webcool_chmod(path, mode)
#define fopen(path, mode) webcool_fopen(path, mode)
#define lstat(path, st) webcool_stat(path, st)
#define mkdir(path, mode) webcool_mkdir(path, mode)
#define rename(old_path, new_path) webcool_rename(old_path, new_path)
#define rmdir(path) webcool_rmdir(path)
#define stat(path, st) webcool_stat(path, st)
#define unlink(path) webcool_unlink(path)

#endif // _WIN32
