#pragma once
#include <cerrno>
#include <cstdlib>
#include <string>
#include <vector>

namespace webcool
{
namespace ai
{
namespace record_codec
{
// Shared wire primitives for the existing versioned text records.
inline int hex_value(char ch)
{
	if (ch >= '0' && ch <= '9')
		return ch - '0';
	if (ch >= 'a' && ch <= 'f')
		return ch - 'a' + 10;
	if (ch >= 'A' && ch <= 'F')
		return ch - 'A' + 10;
	return -1;
}
inline std::string hex_encode(const unsigned char *data, size_t size)
{
	static const char *digits = "0123456789abcdef";
	std::string out;
	out.reserve(size * 2);
	for (size_t i = 0; i < size; ++i) {
		out.push_back(digits[data[i] >> 4]);
		out.push_back(digits[data[i] & 15]);
	}
	return out;
}
inline std::string hex_encode(const std::string &value)
{
	return hex_encode(reinterpret_cast<const unsigned char *>(value.data()),
			  value.size());
}
// Clears output before validation; on an invalid pair the decoded prefix remains.
// Callers whose legacy contract preserves output for odd lengths check first.
inline bool hex_decode(const std::string &value, std::string &output)
{
	output.clear();
	if (value.size() % 2 != 0)
		return false;
	output.reserve(value.size() / 2);
	for (size_t i = 0; i < value.size(); i += 2) {
		const int high = hex_value(value[i]),
			  low = hex_value(value[i + 1]);
		if (high < 0 || low < 0)
			return false;
		output.push_back(static_cast<char>((high << 4) | low));
	}
	return true;
}
inline void split_tabs(const std::string &line,
		       std::vector<std::string> &fields)
{
	fields.clear();
	size_t begin = 0;
	for (;;) {
		const size_t end = line.find('\t', begin);
		fields.push_back(line.substr(begin, end == std::string::npos ?
							    std::string::npos :
							    end - begin));
		if (end == std::string::npos)
			return;
		begin = end + 1;
	}
}
// Keep legacy strtoll grammar and preserve output on failure.
inline bool parse_nonnegative_number(const std::string &text, long long &value)
{
	char *end = NULL;
	errno = 0;
	const long long parsed = strtoll(text.c_str(), &end, 10);
	if (errno != 0 || end == text.c_str() || *end != '\0' || parsed < 0)
		return false;
	value = parsed;
	return true;
}
}
}
}
