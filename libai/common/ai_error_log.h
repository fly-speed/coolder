#pragma once

#include "stdafx.h"

#include <string>

namespace webcool
{
namespace ai
{

// Convert an error intended for the browser into a single bounded log line.
//
// Callers must still avoid passing secrets or untrusted payloads such as API
// keys, prompts, source files, model replies and process output.  This helper
// only removes control characters and bounds the length; it cannot determine
// whether otherwise printable text is sensitive.
inline std::string safe_error_for_log(const std::string &error)
{
	const size_t kMaxLoggedErrorBytes = 512;
	std::string safe;
	safe.reserve(error.size() < kMaxLoggedErrorBytes ?
	        error.size() :
	        kMaxLoggedErrorBytes);
	for (size_t i = 0;
	     i < error.size() && safe.size() < kMaxLoggedErrorBytes; ++i) {
		const unsigned char ch = static_cast<unsigned char>(error[i]);
		if (ch == '\r' || ch == '\n' || ch == '\t')
			safe.push_back(' ');
		else if (ch >= 32 && ch != 127)
			safe.push_back(static_cast<char>(ch));
	}
	return safe.empty() ? "unspecified error" : safe;
}

// Log at the point where a C++ operation decides to propagate a failure.
// Returning false makes the common `return ai_error(...)` pattern concise and
// keeps the UI-facing error string unchanged.
inline bool ai_error(
    const char *component, const char *operation, const std::string &error)
{
	const std::string safe = safe_error_for_log(error);
	logger_error("AI component=%s operation=%s error=%s",
	    component ? component : "unknown",
	    operation ? operation : "unknown", safe.c_str());
	return false;
}

// Some asynchronous operations cannot return false from their current scope.
// Use the void form for exception handlers and background-task finalization.
inline void ai_log_error(
    const char *component, const char *operation, const std::string &error)
{
	(void)ai_error(component, operation, error);
}

} // namespace ai
} // namespace webcool
