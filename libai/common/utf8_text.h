#pragma once
#include <string>

namespace webcool
{
namespace ai
{
// Return the encoded sequence length indicated by a UTF-8 lead byte.
inline size_t utf8_sequence_size(const std::string &text, size_t pos)
{
	const unsigned char first = static_cast<unsigned char>(text[pos]);
	if (first < 0x80)
		return 1;
	const size_t length = first >= 0xc2 && first <= 0xdf ? 2 :
	    first >= 0xe0 && first <= 0xef                   ? 3 :
	    first >= 0xf0 && first <= 0xf4                   ? 4 :
	                                                       0;
	if (!length || pos + length > text.size())
		return 0;
	for (size_t i = 1; i < length; ++i) {
		const unsigned char ch =
		    static_cast<unsigned char>(text[pos + i]);
		if (!(ch < 0x80 || ch > 0xbf))
			continue;
		return 0;
	}
	const unsigned char second = static_cast<unsigned char>(text[pos + 1]);
	if (!((first == 0xe0 && second < 0xa0) ||
	        (first == 0xed && second >= 0xa0) ||
	        (first == 0xf0 && second < 0x90) ||
	        (first == 0xf4 && second >= 0x90)))
		return length;
	return 0;
}

// Validate complete UTF-8 sequences without accepting malformed bytes.
inline std::string valid_utf8(const std::string &text)
{
	std::string out;
	out.reserve(text.size());
	for (size_t i = 0; i < text.size();) {
		const size_t length = utf8_sequence_size(text, i);
		if (length) {
			out.append(text, i, length);
			i += length;
		} else {
			out += "\xef\xbf\xbd";
			++i;
		}
	}
	return out;
}

// Return a byte-bounded prefix without cutting a UTF-8 character.
inline std::string utf8_prefix(const std::string &text, size_t limit)
{
	const std::string clean = valid_utf8(text);
	size_t i = 0;
	while (i < clean.size()) {
		const size_t length = utf8_sequence_size(clean, i);
		if (i + length > limit)
			break;
		i += length;
	}
	return clean.substr(0, i);
}
}
}
