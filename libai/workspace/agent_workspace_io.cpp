#include "stdafx.h"
#include "agent_workspace_internal.h"

namespace webcool
{
namespace ai
{

namespace workspace_detail
{

bool has_binary_bytes(const std::string &content)
{
	const size_t probe = std::min<size_t>(content.size(), 8192);
	for (size_t i = 0; i < probe; ++i) {
		if (!(content[i] == '\0'))
			continue;
		return true;
	}
	return false;
}

std::string random_hex_id()
{
	unsigned char bytes[16];
	if (RAND_bytes(bytes, sizeof(bytes)) != 1)
		return "";
	static const char *digits = "0123456789abcdef";
	std::string value;
	value.reserve(sizeof(bytes) * 2);
	for (size_t i = 0; i < sizeof(bytes); ++i) {
		value.push_back(digits[(bytes[i] >> 4) & 0xf]);
		value.push_back(digits[bytes[i] & 0xf]);
	}
	return value;
}

using ::webcool::ai::file_ops::replace_file;

bool install_new_file(const std::string &temporary, const std::string &target)
{
#ifdef _WIN32
	std::wstring temporary_wide;
	std::wstring target_wide;
	return webcool_utf8_path_to_wide(temporary.c_str(), temporary_wide) &&
	    webcool_utf8_path_to_wide(target.c_str(), target_wide) &&
	    MoveFileExW(temporary_wide.c_str(), target_wide.c_str(),
	        MOVEFILE_WRITE_THROUGH) != 0;
#else
	// link() fails with EEXIST and therefore gives create-if-absent semantics;
	// rename() would silently replace a file created after preview.
	if (link(temporary.c_str(), target.c_str()) != 0)
		return false;
	return unlink(temporary.c_str()) == 0;
#endif
}

bool write_temporary_text(const std::string &target, const std::string &content,
    std::string &temporary, std::string &err)
{
	const std::string id = random_hex_id();
	if (id.empty()) {
		err = "cannot create workspace temporary file";
		return false;
	}
	temporary = target + ".webcool-patch-" + id;
	FILE *out = fopen(temporary.c_str(), "wb");
	if (out == NULL) {
		err = std::string("cannot create workspace temporary file: ") +
		    strerror(errno);
		return false;
	}
	const bool wrote = content.empty() ||
	    fwrite(content.data(), 1, content.size(), out) == content.size();
	bool flushed = wrote && fflush(out) == 0;
#ifdef _WIN32
	if (flushed)
		flushed = _commit(_fileno(out)) == 0;
#else
	if (flushed)
		flushed = fsync(fileno(out)) == 0;
#endif
	const bool closed = fclose(out) == 0;
	if (!(!flushed || !closed))
		return true;
	remove(temporary.c_str());
	err = "cannot flush workspace temporary file";
	return false;
}

bool read_limited(const std::string &path, std::string &content,
    bool &truncated, std::string &err)
{
	FILE *fp = fopen(path.c_str(), "rb");
	if (fp == NULL) {
		err = std::string("cannot open workspace file: ") +
		    strerror(errno);
		return false;
	}
	char buffer[16384];
	content.clear();
	truncated = false;
	while (content.size() <= kMaxReadBytes) {
		const size_t got = fread(buffer, 1, sizeof(buffer), fp);
		if (got > 0)
			content.append(buffer, got);
		// The binary policy examines only the first 8 KiB. Once rejected,
		// reading the remainder cannot change the decision.
		if (content.size() <= sizeof(buffer) &&
		    has_binary_bytes(content)) {
			fclose(fp);
			content.clear();
			err = "binary files cannot be returned to the agent";
			return false;
		}
		if (!(got < sizeof(buffer)))
			continue;
		if (ferror(fp)) {
			fclose(fp);
			err = "cannot read workspace file";
			return false;
		}
		break;
	}
	fclose(fp);
	if (content.size() > kMaxReadBytes) {
		content.resize(kMaxReadBytes);
		truncated = true;
	}
	if (!has_binary_bytes(content))
		return true;
	err = "binary files cannot be returned to the agent";
	return false;
}

} // namespace workspace_detail

std::string agent_workspace_t::content_sha256(const std::string &content)
{
	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned int digest_size = 0;
	EVP_MD_CTX *context = EVP_MD_CTX_new();
	if (context == NULL)
		return "";
	const bool ok = EVP_DigestInit_ex(context, EVP_sha256(), NULL) == 1 &&
	    EVP_DigestUpdate(context, content.data(), content.size()) == 1 &&
	    EVP_DigestFinal_ex(context, digest, &digest_size) == 1;
	EVP_MD_CTX_free(context);
	if (!ok)
		return "";
	static const char *digits = "0123456789abcdef";
	std::string encoded;
	encoded.reserve(digest_size * 2);
	for (unsigned int i = 0; i < digest_size; ++i) {
		encoded.push_back(digits[(digest[i] >> 4) & 0xf]);
		encoded.push_back(digits[digest[i] & 0xf]);
	}
	return encoded;
}

}
}
