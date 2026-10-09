#pragma once
#include "stdafx.h"
#include "../common/file_ops.h"
#include "../common/identifiers.h"
#include "../common/record_codec.h"
#include "ai_provider_store.h"
#include "../common/ai_error_log.h"
#include "storage_paths.h"
#ifdef _WIN32
#include "../common/platform_compat.h"
#else
#include <dirent.h>
#include <unistd.h>
#endif

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include "../common/webcool_mutex.h"

namespace webcool
{
namespace ai
{
namespace provider_store_detail
{

// providers.v1 contains metadata plus AES-256-GCM ciphertext. The 32-byte
// installation master key is separate from every user's provider file, and
// additional authenticated data binds ciphertext to username + provider ID.
extern webcool::mutex g_provider_store_mutex;
extern const char *kAgentDir;
extern const char *kProviderFile;
extern const char *kProviderHealthFile;
extern const char *kMasterKeyFile;
extern const char *kHeader;
extern const char *kHealthHeader;
extern const char *kMasterKeyHeader;

struct master_keyring_t {
	std::vector<unsigned char> active;
	std::vector<unsigned char> secondary;
	std::string phase;

	~master_keyring_t()
	{
		if (!active.empty())
			OPENSSL_cleanse(&active[0], active.size());
		if (!secondary.empty())
			OPENSSL_cleanse(&secondary[0], secondary.size());
	}
};

struct cleanse_vector_t {
	explicit cleanse_vector_t(std::vector<unsigned char> &value)
	        : value_(value)
	{
	}
	~cleanse_vector_t()
	{
		if (!value_.empty())
			OPENSSL_cleanse(&value_[0], value_.size());
	}
	std::vector<unsigned char> &value_;
};

using ::webcool::ai::record_codec::hex_encode;

using ::webcool::ai::record_codec::hex_value;

using ::webcool::ai::record_codec::hex_decode;

using ::webcool::ai::record_codec::split_tabs;

std::string lower_ascii(std::string value);

using ::webcool::ai::identifiers::valid_id;

using ::webcool::ai::identifiers::new_id;

bool ensure_private_dir(const std::string &path, std::string &err);

bool write_master_key_file(const std::string &upload_root,
    const std::vector<unsigned char> &active,
    const std::vector<unsigned char> &secondary, const std::string &phase,
    std::string &err);

bool decode_key_text(
    const std::string &encoded, std::vector<unsigned char> &key);

bool read_master_keyring(const std::string &upload_root,
    master_keyring_t &keyring, std::string &err);

bool encrypt_secret_with_key(const std::vector<unsigned char> &key,
    const std::string &aad, const std::string &plaintext, std::string &encoded,
    std::string &err);

bool encrypt_secret(const std::string &upload_root, const std::string &aad,
    const std::string &plaintext, std::string &encoded, std::string &err);

bool decrypt_secret_with_key(const std::vector<unsigned char> &key,
    const std::string &aad, const std::string &encoded, std::string &plaintext,
    std::string &err);

bool decrypt_secret(const std::string &upload_root, const std::string &aad,
    const std::string &encoded, std::string &plaintext, std::string &err);

std::string agent_dir(const std::string &user_root);

std::string provider_file(const std::string &user_root);

std::string provider_health_file(const std::string &user_root);

struct rotation_user_t {
	std::string username;
	std::string user_root;
};

bool valid_rotation_username(const std::string &username);

using ::webcool::ai::file_ops::safe_directory;

bool safe_regular_file(const std::string &path);

using ::webcool::ai::file_ops::path_entry_exists;

// A restart checkpoint is encrypted with the current provider keyring. Retiring
// either key while a checkpoint exists could make an interrupted run
// unrecoverable, so master-key rotation is blocked until those runs finish or
// are cancelled. The scan treats any entry as material and fails closed.
bool checkpoint_directory_has_entries(
    const std::string &user_root, bool &has_entries, std::string &err);

bool collect_rotation_users(const std::string &upload_root,
    std::vector<rotation_user_t> &users, std::string &err);

bool parse_nonnegative_number(const std::string &text, long long &value);

// Test failures sometimes contain line breaks or very large upstream response
// fragments. Keep only a compact display-safe summary in the user's telemetry.
std::string sanitize_test_error(const std::string &input);

bool load_unlocked(const std::string &user_root,
    std::vector<provider_config_t> &providers, std::string &err);

bool load_health_unlocked(const std::string &user_root,
    std::vector<provider_config_t> &providers, std::string &err);

bool save_health_unlocked(const std::string &user_root,
    const std::vector<provider_config_t> &providers, std::string &err);

bool save_unlocked(const std::string &user_root,
    const std::vector<provider_config_t> &providers, std::string &err);

bool protocol_supported(const std::string &protocol);

bool parse_base_url(const std::string &url, bool &https, std::string &hostname,
    std::string &err);

bool is_loopback_host(const std::string &host);

}
}
}
