
#pragma once

#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

//#include <iostream>
//#include <tchar.h>

#include "lib_acl.h"
#include "acl_cpp/lib_acl.hpp"
#include "fiber/lib_fiber.hpp"

#ifdef WIN32
#define snprintf _snprintf
#endif

#undef logger
#undef logger_warn
#undef logger_error
#undef logger_fatal
#undef logger_debug

#if defined(_WIN32) || defined(_WIN64)

#if _MSC_VER >= 1500
#define logger(fmt, ...) \
	acl::log::msg4(__FILE__, __LINE__, __FUNCTION__, fmt, __VA_ARGS__)
#define logger_warn(fmt, ...) \
	acl::log::warn4(__FILE__, __LINE__, __FUNCTION__, fmt, __VA_ARGS__)
#define logger_error(fmt, ...) \
	acl::log::error4(__FILE__, __LINE__, __FUNCTION__, fmt, __VA_ARGS__)
#define logger_fatal(fmt, ...) \
	acl::log::fatal4(__FILE__, __LINE__, __FUNCTION__, fmt, __VA_ARGS__)
#define logger_debug(section, level, fmt, ...)                                \
	acl::log::msg6(section, level, __FILE__, __LINE__, __FUNCTION__, fmt, \
		       __VA_ARGS__)
#else
#define logger acl::log::msg1
#define logger_warn acl::log::warn1
#define logger_error acl::log::error1
#define logger_fatal acl::log::fatal1
#define logger_debug acl::log::msg3
#endif
#else
#define logger(fmt, args...) \
	acl::log::msg4(__FILE__, __LINE__, __FUNCTION__, fmt, ##args)
#define logger_warn(fmt, args...) \
	acl::log::warn4(__FILE__, __LINE__, __FUNCTION__, fmt, ##args)
#define logger_error(fmt, args...) \
	acl::log::error4(__FILE__, __LINE__, __FUNCTION__, fmt, ##args)
#define logger_fatal(fmt, args...) \
	acl::log::fatal4(__FILE__, __LINE__, __FUNCTION__, fmt, ##args)
#define logger_debug(section, level, fmt, args...)                            \
	acl::log::msg6(section, level, __FILE__, __LINE__, __FUNCTION__, fmt, \
		       ##args)
#endif // !_WIN32 && !_WIN64

#define DEBUG_MIN 200
#define DEBUG_FILE (DEBUG_MIN + 1)
#define DEBUG_FOLDER (DEBUG_MIN + 2)
#define DEBUG_CONN (DEBUG_MIN + 3)
#define DEBUG_PAGE (DEBUG_MIN + 4)
#define DEBUG_UPLOAD (DEBUG_MIN + 5)
#define DEBUG_MIME (DEBUG_MIN + 6)
#define DEBUG_ACTION (DEBUG_MIN + 7)
