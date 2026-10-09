#pragma once
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>
#include <openssl/sha.h>

namespace webcool
{
namespace ai
{
inline std::string output_limit_cache_path(
    const std::string &directory, const std::string &identity)
{
	if (directory.empty())
		return "";
	unsigned char digest[SHA256_DIGEST_LENGTH];
	SHA256(reinterpret_cast<const unsigned char *>(identity.data()),
	    identity.size(), digest);
	const char *hex = "0123456789abcdef";
	std::string name;
	for (size_t i = 0; i < sizeof(digest); ++i) {
		name += hex[digest[i] >> 4];
		name += hex[digest[i] & 15];
	}
	return directory + "/output-limit-" + name + ".v1";
}
inline long long load_output_limit_cache(const std::string &path, long long now)
{
	if (path.empty())
		return 0;
	std::ifstream in(path.c_str());
	std::string version, extra;
	long long timestamp = 0, limit = 0;
	if (!(!(in >> version >> timestamp >> limit) ||
	        version != "WEBCOOL_OUTPUT_LIMIT_V1" || (in >> extra) ||
	        limit < 1 || limit > 1000000 || timestamp > now ||
	        timestamp < now - 30LL * 24 * 3600))
		return limit;
	return 0;
}
// The caller serializes writes. Store only timestamp + learned limit; endpoint,
// credentials, prompts and responses are never persisted in this cache.
inline bool save_output_limit_cache(
    const std::string &path, long long limit, long long now)
{
	if (path.empty())
		return true;
	const std::string temporary = path + ".tmp";
	std::ofstream out(temporary.c_str(), std::ios::out | std::ios::trunc);
	out << "WEBCOOL_OUTPUT_LIMIT_V1\n" << now << "\n" << limit << "\n";
	out.close();
	if (!(!out || std::rename(temporary.c_str(), path.c_str()) != 0))
		return true;
	std::remove(temporary.c_str());
	return false;
}
}
}
