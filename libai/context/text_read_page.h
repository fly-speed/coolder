#pragma once
#include "../common/utf8_text.h"

namespace webcool
{
namespace ai
{
// One bounded source page with continuation and content identity metadata.
struct text_read_page_t {
	// Text payload associated with this operation.
	std::string content;
	// Byte offset identifying the start of a retained source page.
	size_t offset = 0;
	// Byte offset at which the next source page should begin.
	size_t next_offset = 0;
	// Total byte length of the complete source content.
	size_t total_bytes = 0;
	// Whether this page reaches the end of the source content.
	bool eof = false;
};
// Read a bounded source page and provide its continuation metadata.
inline bool read_text_page(const std::string &content, const std::string &query,
    text_read_page_t &page, std::string &error, size_t chunk_bytes = 8192,
    size_t file_limit_bytes = 1024 * 1024,
    bool prefer_complete_small_file = false)
{
	page = text_read_page_t();
	if (chunk_bytes < 4 || chunk_bytes > 65536 ||
	    file_limit_bytes > 1024 * 1024 || file_limit_bytes < chunk_bytes) {
		error = "invalid read limits";
		return false;
	}
	if (content.size() > file_limit_bytes) {
		error = "text file exceeds configured read file limit";
		return false;
	}
	size_t offset = 0;
	for (size_t i = 0; i < query.size(); ++i) {
		if (query[i] < '0' || query[i] > '9' ||
		    offset > 1024 * 1024 / 10) {
			error =
			    "read query must be a decimal byte offset from next_query";
			return false;
		}
		offset = offset * 10 + static_cast<size_t>(query[i] - '0');
	}
	if (offset > content.size()) {
		error = "read offset exceeds file size";
		return false;
	}
	if (offset < content.size() &&
	    (static_cast<unsigned char>(content[offset]) & 0xc0) == 0x80) {
		error = "read offset splits a UTF-8 character; use next_query";
		return false;
	}
	// Keep explicit offset validation, but return a complete small file in one page.
	if (prefer_complete_small_file && content.size() <= chunk_bytes)
		offset = 0;
	size_t end = offset;
	while (end < content.size()) {
		const size_t sequence = utf8_sequence_size(content, end);
		const size_t length = sequence ? sequence : 1;
		if (end - offset + length > chunk_bytes)
			break;
		end += length;
	}
	page.content = content.substr(offset, end - offset);
	page.offset = offset;
	page.next_offset = end;
	page.total_bytes = content.size();
	page.eof = end == content.size();
	return true;
}
}
}
