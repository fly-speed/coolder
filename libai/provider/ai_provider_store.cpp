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
namespace
{

// providers.v1 contains metadata plus AES-256-GCM ciphertext. The 32-byte
// installation master key is separate from every user's provider file, and
// additional authenticated data binds ciphertext to username + provider ID.
webcool::mutex g_provider_store_mutex;
const char *kAgentDir = ".webcool_agent";
const char *kProviderFile = "providers.v1";
const char *kProviderHealthFile = "provider_health.v1";
const char *kMasterKeyFile = "webcool_auth/ai_master.key";
const char *kHeader = "WEBCOOL_AI_PROVIDERS_V1";
const char *kHealthHeader = "WEBCOOL_AI_PROVIDER_HEALTH_V1";
const char *kMasterKeyHeader = "WEBCOOL_AI_MASTER_KEY_V2";

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

std::string lower_ascii(std::string value)
{
	for (size_t i = 0; i < value.size(); ++i) {
		value[i] = static_cast<char>(
			::tolower(static_cast<unsigned char>(value[i])));
	}
	return value;
}

using ::webcool::ai::identifiers::valid_id;

using ::webcool::ai::identifiers::new_id;

bool ensure_private_dir(const std::string &path, std::string &err)
{
	if (!storage_detail::make_dir_recursive(path.c_str())) {
		err = "cannot create AI settings directory";
		return false;
	}
#ifdef _WIN32
	std::wstring wide;
	if (!webcool_utf8_path_to_wide(path.c_str(), wide)) {
		err = "cannot validate AI settings directory";
		return false;
	}
	const DWORD attributes = GetFileAttributesW(wide.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES ||
	    (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
	    (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
		err = "AI settings path is not a safe directory";
		return false;
	}
#else
	struct stat st;
	if (lstat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) ||
	    S_ISLNK(st.st_mode)) {
		err = "AI settings path is not a safe directory";
		return false;
	}
	if (chmod(path.c_str(), 0700) != 0) {
		err = std::string("cannot protect AI settings directory: ") +
		      strerror(errno);
		return false;
	}
#endif
	return true;
}

bool write_master_key_file(const std::string &upload_root,
			   const std::vector<unsigned char> &active,
			   const std::vector<unsigned char> &secondary,
			   const std::string &phase, std::string &err)
{
	if (active.size() != 32 ||
	    (!secondary.empty() && secondary.size() != 32) ||
	    (!secondary.empty() && phase != "prepared" &&
	     phase != "committed")) {
		err = "invalid AI master key journal state";
		return false;
	}
	const std::string auth_dir =
		storage_detail::join_path(upload_root, "webcool_auth");
	if (!ensure_private_dir(auth_dir, err))
		return false;
	const std::string path =
		storage_detail::join_path(upload_root, kMasterKeyFile);
	const std::string tmp = path + ".tmp";
	std::ofstream out(tmp.c_str(),
			  std::ios::out | std::ios::trunc | std::ios::binary);
	if (!out.good()) {
		err = "cannot write AI master key";
		return false;
	}
	if (secondary.empty()) {
		out << hex_encode(&active[0], active.size()) << '\n';
	} else {
		out << kMasterKeyHeader << '\n'
		    << "active=" << hex_encode(&active[0], active.size())
		    << '\n'
		    << "secondary="
		    << hex_encode(&secondary[0], secondary.size()) << '\n'
		    << "phase=" << phase << '\n';
	}
	out.close();
	if (!out.good()) {
		err = "cannot flush AI master key";
		return false;
	}
#ifndef _WIN32
	if (chmod(tmp.c_str(), 0600) != 0) {
		unlink(tmp.c_str());
		err = "cannot protect AI master key";
		return false;
	}
#endif
	if (rename(tmp.c_str(), path.c_str()) != 0) {
		err = std::string("cannot install AI master key: ") +
		      strerror(errno);
		return false;
	}
	return true;
}

bool decode_key_text(const std::string &encoded,
		     std::vector<unsigned char> &key)
{
	std::string decoded;
	if (!hex_decode(encoded, decoded) || decoded.size() != 32) {
		if (!decoded.empty())
			OPENSSL_cleanse(&decoded[0], decoded.size());
		return false;
	}
	key.assign(decoded.begin(), decoded.end());
	OPENSSL_cleanse(&decoded[0], decoded.size());
	return true;
}

bool read_master_keyring(const std::string &upload_root,
			 master_keyring_t &keyring, std::string &err)
{
	const std::string auth_dir =
		storage_detail::join_path(upload_root, "webcool_auth");
	if (!ensure_private_dir(auth_dir, err))
		return false;
	const std::string path =
		storage_detail::join_path(upload_root, kMasterKeyFile);
	std::ifstream in(path.c_str(), std::ios::in | std::ios::binary);
	if (!in.good()) {
		keyring.active.assign(32, 0);
		if (RAND_bytes(&keyring.active[0],
			       static_cast<int>(keyring.active.size())) != 1) {
			err = "cannot generate AI master key";
			return false;
		}
		return write_master_key_file(upload_root, keyring.active,
					     keyring.secondary, keyring.phase,
					     err);
	}
	std::string first;
	if (!std::getline(in, first)) {
		err = "invalid AI master key";
		return false;
	}
	if (first != kMasterKeyHeader) {
		if (!decode_key_text(first, keyring.active)) {
			err = "invalid AI master key";
			return false;
		}
		return true;
	}
	std::string active_line;
	std::string secondary_line;
	std::string phase_line;
	std::string extra;
	if (!std::getline(in, active_line) ||
	    !std::getline(in, secondary_line) ||
	    !std::getline(in, phase_line) || std::getline(in, extra) ||
	    active_line.compare(0, 7, "active=") != 0 ||
	    secondary_line.compare(0, 10, "secondary=") != 0 ||
	    phase_line.compare(0, 6, "phase=") != 0 ||
	    !decode_key_text(active_line.substr(7), keyring.active) ||
	    !decode_key_text(secondary_line.substr(10), keyring.secondary)) {
		err = "invalid AI master key journal";
		return false;
	}
	keyring.phase = phase_line.substr(6);
	if (keyring.phase != "prepared" && keyring.phase != "committed") {
		err = "invalid AI master key journal phase";
		return false;
	}
	return true;
}

bool encrypt_secret_with_key(const std::vector<unsigned char> &key,
			     const std::string &aad,
			     const std::string &plaintext, std::string &encoded,
			     std::string &err)
{
	if (key.size() != 32) {
		err = "invalid AI encryption key";
		return false;
	}
	unsigned char nonce[12];
	if (RAND_bytes(nonce, sizeof(nonce)) != 1) {
		err = "cannot generate API key nonce";
		return false;
	}
	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	if (ctx == NULL) {
		err = "cannot initialize API key encryption";
		return false;
	}
	std::vector<unsigned char> ciphertext(plaintext.size() + 16);
	unsigned char tag[16];
	int count = 0;
	int total = 0;
	bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL,
				     NULL) == 1 &&
		  EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
				      sizeof(nonce), NULL) == 1 &&
		  EVP_EncryptInit_ex(ctx, NULL, NULL, &key[0], nonce) == 1;
	if (ok && !aad.empty()) {
		ok = EVP_EncryptUpdate(ctx, NULL, &count,
				       reinterpret_cast<const unsigned char *>(
					       aad.data()),
				       static_cast<int>(aad.size())) == 1;
	}
	if (ok && !plaintext.empty()) {
		ok = EVP_EncryptUpdate(ctx, &ciphertext[0], &count,
				       reinterpret_cast<const unsigned char *>(
					       plaintext.data()),
				       static_cast<int>(plaintext.size())) == 1;
		total = count;
	}
	if (ok) {
		ok = EVP_EncryptFinal_ex(ctx, &ciphertext[0] + total, &count) ==
		     1;
		total += count;
	}
	if (ok)
		ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, sizeof(tag),
					 tag) == 1;
	EVP_CIPHER_CTX_free(ctx);
	if (!ok) {
		err = "cannot encrypt API key";
		return false;
	}
	std::string packed("v1:");
	packed += hex_encode(nonce, sizeof(nonce));
	packed += ":";
	packed += hex_encode(tag, sizeof(tag));
	packed += ":";
	packed += hex_encode(&ciphertext[0], static_cast<size_t>(total));
	encoded.swap(packed);
	return true;
}

bool encrypt_secret(const std::string &upload_root, const std::string &aad,
		    const std::string &plaintext, std::string &encoded,
		    std::string &err)
{
	master_keyring_t keyring;
	if (!read_master_keyring(upload_root, keyring, err))
		return false;
	return encrypt_secret_with_key(keyring.active, aad, plaintext, encoded,
				       err);
}

bool decrypt_secret_with_key(const std::vector<unsigned char> &key,
			     const std::string &aad, const std::string &encoded,
			     std::string &plaintext, std::string &err)
{
	plaintext.clear();
	if (encoded.empty())
		return true;
	if (encoded.compare(0, 3, "v1:") != 0) {
		err = "unsupported API key encryption version";
		return false;
	}
	const size_t first = encoded.find(':', 3);
	const size_t second = first == std::string::npos ?
				      std::string::npos :
				      encoded.find(':', first + 1);
	if (first == std::string::npos || second == std::string::npos) {
		err = "invalid encrypted API key";
		return false;
	}
	std::string nonce_text, tag_text, ciphertext_text;
	if (!hex_decode(encoded.substr(3, first - 3), nonce_text) ||
	    !hex_decode(encoded.substr(first + 1, second - first - 1),
			tag_text) ||
	    !hex_decode(encoded.substr(second + 1), ciphertext_text) ||
	    nonce_text.size() != 12 || tag_text.size() != 16) {
		err = "invalid encrypted API key";
		return false;
	}
	if (key.size() != 32) {
		err = "invalid AI decryption key";
		return false;
	}
	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	if (ctx == NULL) {
		err = "cannot initialize API key decryption";
		return false;
	}
	std::vector<unsigned char> clear(ciphertext_text.size() + 1);
	cleanse_vector_t clear_guard(clear);
	int count = 0;
	int total = 0;
	bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL,
				     NULL) == 1 &&
		  EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
				      static_cast<int>(nonce_text.size()),
				      NULL) == 1 &&
		  EVP_DecryptInit_ex(ctx, NULL, NULL, &key[0],
				     reinterpret_cast<const unsigned char *>(
					     nonce_text.data())) == 1;
	if (ok && !aad.empty()) {
		ok = EVP_DecryptUpdate(ctx, NULL, &count,
				       reinterpret_cast<const unsigned char *>(
					       aad.data()),
				       static_cast<int>(aad.size())) == 1;
	}
	if (ok && !ciphertext_text.empty()) {
		ok = EVP_DecryptUpdate(
			     ctx, &clear[0], &count,
			     reinterpret_cast<const unsigned char *>(
				     ciphertext_text.data()),
			     static_cast<int>(ciphertext_text.size())) == 1;
		total = count;
	}
	if (ok) {
		ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG,
					 static_cast<int>(tag_text.size()),
					 const_cast<char *>(tag_text.data())) ==
		     1;
	}
	if (ok) {
		ok = EVP_DecryptFinal_ex(ctx, &clear[0] + total, &count) == 1;
		total += count;
	}
	EVP_CIPHER_CTX_free(ctx);
	if (!ok) {
		err = "cannot decrypt API key";
		return false;
	}
	plaintext.assign(reinterpret_cast<const char *>(&clear[0]),
			 static_cast<size_t>(total));
	return true;
}

bool decrypt_secret(const std::string &upload_root, const std::string &aad,
		    const std::string &encoded, std::string &plaintext,
		    std::string &err)
{
	master_keyring_t keyring;
	if (!read_master_keyring(upload_root, keyring, err))
		return false;
	if (decrypt_secret_with_key(keyring.active, aad, encoded, plaintext,
				    err)) {
		return true;
	}
	if (!keyring.secondary.empty() &&
	    decrypt_secret_with_key(keyring.secondary, aad, encoded, plaintext,
				    err)) {
		return true;
	}
	err = "cannot decrypt API key";
	return false;
}

std::string agent_dir(const std::string &user_root)
{
	return storage_detail::join_path(user_root, kAgentDir);
}

std::string provider_file(const std::string &user_root)
{
	return storage_detail::join_path(agent_dir(user_root), kProviderFile);
}

std::string provider_health_file(const std::string &user_root)
{
	return storage_detail::join_path(agent_dir(user_root),
					 kProviderHealthFile);
}

struct rotation_user_t {
	std::string username;
	std::string user_root;
};

bool valid_rotation_username(const std::string &username)
{
	if (username.size() < 3 || username.size() > 40)
		return false;
	for (size_t i = 0; i < username.size(); ++i) {
		const char c = username[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '_' || c == '-' ||
		      c == '.')) {
			return false;
		}
	}
	return username != "." && username != "..";
}

using ::webcool::ai::file_ops::safe_directory;

bool safe_regular_file(const std::string &path)
{
#ifdef _WIN32
	std::wstring wide;
	if (!webcool_utf8_path_to_wide(path.c_str(), wide))
		return false;
	const DWORD attributes = GetFileAttributesW(wide.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES &&
	       (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
	       (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
#else
	struct stat st;
	return lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
	       !S_ISLNK(st.st_mode);
#endif
}

using ::webcool::ai::file_ops::path_entry_exists;

// A restart checkpoint is encrypted with the current provider keyring. Retiring
// either key while a checkpoint exists could make an interrupted run
// unrecoverable, so master-key rotation is blocked until those runs finish or
// are cancelled. The scan treats any entry as material and fails closed.
bool checkpoint_directory_has_entries(const std::string &user_root,
				      bool &has_entries, std::string &err)
{
	has_entries = false;
	const std::string path = storage_detail::join_path(
		user_root, ".webcool_agent/checkpoints");
	if (!path_entry_exists(path))
		return true;
	if (!safe_directory(path)) {
		err = "agent checkpoint storage is not a safe directory";
		return false;
	}
#ifdef _WIN32
	std::wstring wide;
	if (!webcool_utf8_path_to_wide((path + "/*").c_str(), wide)) {
		err = "cannot validate agent checkpoint storage";
		return false;
	}
	WIN32_FIND_DATAW data;
	HANDLE find = FindFirstFileW(wide.c_str(), &data);
	if (find == INVALID_HANDLE_VALUE) {
		const DWORD code = GetLastError();
		if (code == ERROR_FILE_NOT_FOUND)
			return true;
		err = "cannot enumerate agent checkpoint storage";
		return false;
	}
	do {
		const std::wstring name(data.cFileName);
		if (name != L"." && name != L"..") {
			has_entries = true;
			break;
		}
	} while (FindNextFileW(find, &data) != 0);
	FindClose(find);
#else
	DIR *directory = opendir(path.c_str());
	if (directory == NULL) {
		err = std::string(
			      "cannot enumerate agent checkpoint storage: ") +
		      strerror(errno);
		return false;
	}
	for (;;) {
		struct dirent *entry = readdir(directory);
		if (entry == NULL)
			break;
		const std::string name(entry->d_name);
		if (name != "." && name != "..") {
			has_entries = true;
			break;
		}
	}
	closedir(directory);
#endif
	return true;
}

bool collect_rotation_users(const std::string &upload_root,
			    std::vector<rotation_user_t> &users,
			    std::string &err)
{
	users.clear();
	const std::string root =
		storage_detail::join_path(upload_root, "webcool_users");
	struct stat root_stat;
	if (stat(root.c_str(), &root_stat) != 0) {
		if (errno == ENOENT)
			return true;
		err = std::string("cannot inspect AI user storage: ") +
		      strerror(errno);
		return false;
	}
	if (!safe_directory(root)) {
		err = "AI user storage is not a safe directory";
		return false;
	}
	DIR *directory = opendir(root.c_str());
	if (directory == NULL) {
		err = std::string("cannot enumerate AI user storage: ") +
		      strerror(errno);
		return false;
	}
	for (;;) {
		struct dirent *entry = readdir(directory);
		if (entry == NULL)
			break;
		const std::string username(entry->d_name);
		if (!valid_rotation_username(username))
			continue;
		const std::string user_root =
			storage_detail::join_path(root, username);
		if (!safe_directory(user_root)) {
			closedir(directory);
			err = "AI user entry is not a safe directory";
			return false;
		}
		bool has_checkpoints = false;
		if (!checkpoint_directory_has_entries(user_root,
						      has_checkpoints, err)) {
			closedir(directory);
			return false;
		}
		if (has_checkpoints) {
			closedir(directory);
			err = "cannot rotate AI master key while recoverable agent runs exist";
			return false;
		}
		const std::string path = provider_file(user_root);
		if (!path_entry_exists(path))
			continue;
		if (!safe_regular_file(path)) {
			closedir(directory);
			err = "AI provider database is not a safe regular file";
			return false;
		}
		rotation_user_t user;
		user.username = username;
		user.user_root = user_root;
		users.push_back(user);
	}
	closedir(directory);
	std::sort(
		users.begin(), users.end(),
		[](const rotation_user_t &left, const rotation_user_t &right) {
			return left.username < right.username;
		});
	return true;
}

bool parse_nonnegative_number(const std::string &text, long long &value)
{
	if (text.empty())
		return false;
	char *end = NULL;
	errno = 0;
	const long long parsed = std::strtoll(text.c_str(), &end, 10);
	if (errno != 0 || end == NULL || *end != '\0' || parsed < 0)
		return false;
	value = parsed;
	return true;
}

// Test failures sometimes contain line breaks or very large upstream response
// fragments. Keep only a compact display-safe summary in the user's telemetry.
std::string sanitize_test_error(const std::string &input)
{
	std::string output;
	output.reserve(std::min<size_t>(input.size(), 512));
	for (size_t i = 0; i < input.size() && output.size() < 512; ++i) {
		const unsigned char c = static_cast<unsigned char>(input[i]);
		output.push_back(c < 0x20 || c == 0x7f ? ' ' :
							 static_cast<char>(c));
	}
	return output;
}

bool load_unlocked(const std::string &user_root,
		   std::vector<provider_config_t> &providers, std::string &err)
{
	providers.clear();
	std::ifstream in(provider_file(user_root).c_str(),
			 std::ios::in | std::ios::binary);
	if (!in.good())
		return true;
	std::string line;
	if (!std::getline(in, line) || line != kHeader) {
		err = "invalid AI provider database";
		return false;
	}
	while (std::getline(in, line)) {
		if (line.empty())
			continue;
		std::vector<std::string> fields;
		split_tabs(line, fields);
		// The first ten columns are the original V1 record. Optional Responses
		// controls were appended so existing installations remain readable without
		// an eager migration or a second provider database.
		if (fields.size() != 10 && fields.size() != 19 &&
		    fields.size() != 21 && fields.size() != 22) {
			err = "invalid AI provider record";
			return false;
		}
		provider_config_t item;
		if (!hex_decode(fields[0], item.id) ||
		    !hex_decode(fields[1], item.name) ||
		    !hex_decode(fields[2], item.protocol) ||
		    !hex_decode(fields[3], item.base_url) ||
		    !hex_decode(fields[4], item.model) ||
		    !hex_decode(fields[5], item.api_key_ciphertext) ||
		    !hex_decode(fields[6], item.api_key_hint)) {
			err = "invalid AI provider encoding";
			return false;
		}
		item.enabled = fields[7] == "1";
		item.allow_file_content = fields[8] == "1";
		item.is_default = fields[9] == "1";
		if (fields.size() >= 19) {
			item.responses_store = fields[10] == "1";
			item.responses_background = fields[11] == "1";
			item.responses_compact = fields[12] == "1";
			item.responses_strict_tools = fields[13] == "1";
			if (!hex_decode(fields[14],
					item.responses_min_reasoning_effort) ||
			    !hex_decode(fields[15],
					item.responses_reasoning_summary) ||
			    !hex_decode(fields[16],
					item.responses_text_verbosity) ||
			    !hex_decode(fields[17],
					item.responses_service_tier) ||
			    !hex_decode(fields[18], item.responses_cache_ttl)) {
				err = "invalid AI provider Responses settings";
				return false;
			}
		}
		if (fields.size() >= 21 &&
		    (!hex_decode(fields[19], item.openai_organization) ||
		     !hex_decode(fields[20], item.openai_project))) {
			err = "invalid OpenAI routing header encoding";
			return false;
		}
		if (fields.size() == 22)
			item.qwen_session_cache = fields[21] == "1";
		item.output_limit_cache_directory = agent_dir(user_root);
		providers.push_back(item);
	}
	return true;
}

bool load_health_unlocked(const std::string &user_root,
			  std::vector<provider_config_t> &providers,
			  std::string &err)
{
	std::ifstream in(provider_health_file(user_root).c_str(),
			 std::ios::in | std::ios::binary);
	if (!in.good())
		return true;
	std::string line;
	if (!std::getline(in, line) || line != kHealthHeader) {
		err = "invalid AI provider health database";
		return false;
	}
	while (std::getline(in, line)) {
		if (line.empty())
			continue;
		std::vector<std::string> fields;
		split_tabs(line, fields);
		if (fields.size() != 6) {
			err = "invalid AI provider health record";
			return false;
		}
		std::string id;
		std::string status;
		std::string error_summary;
		long long tested_at = 0;
		long long http_status = 0;
		long long latency_ms = 0;
		if (!hex_decode(fields[0], id) ||
		    !hex_decode(fields[1], status) ||
		    !parse_nonnegative_number(fields[2], tested_at) ||
		    !parse_nonnegative_number(fields[3], http_status) ||
		    !parse_nonnegative_number(fields[4], latency_ms) ||
		    !hex_decode(fields[5], error_summary) || !valid_id(id) ||
		    (status != "ok" && status != "error") ||
		    http_status > 999 || error_summary.size() > 512) {
			err = "invalid AI provider health encoding";
			return false;
		}
		for (size_t i = 0; i < providers.size(); ++i) {
			if (providers[i].id != id)
				continue;
			providers[i].last_test_status = status;
			providers[i].last_test_at = tested_at;
			providers[i].last_test_http_status =
				static_cast<int>(http_status);
			providers[i].last_test_latency_ms = latency_ms;
			providers[i].last_test_error = error_summary;
			break;
		}
	}
	return true;
}

bool save_health_unlocked(const std::string &user_root,
			  const std::vector<provider_config_t> &providers,
			  std::string &err)
{
	const std::string dir = agent_dir(user_root);
	if (!ensure_private_dir(dir, err))
		return false;
	const std::string path = provider_health_file(user_root);
	const std::string tmp = path + ".tmp";
	std::ofstream out(tmp.c_str(),
			  std::ios::out | std::ios::trunc | std::ios::binary);
	if (!out.good()) {
		err = "cannot write AI provider health database";
		return false;
	}
	out << kHealthHeader << '\n';
	for (size_t i = 0; i < providers.size(); ++i) {
		const provider_config_t &item = providers[i];
		if (item.last_test_status.empty() || item.last_test_at <= 0)
			continue;
		out << hex_encode(item.id) << '\t'
		    << hex_encode(item.last_test_status) << '\t'
		    << item.last_test_at << '\t' << item.last_test_http_status
		    << '\t' << item.last_test_latency_ms << '\t'
		    << hex_encode(item.last_test_error) << '\n';
	}
	out.close();
	if (!out.good()) {
		err = "cannot flush AI provider health database";
		return false;
	}
#ifndef _WIN32
	if (chmod(tmp.c_str(), 0600) != 0) {
		unlink(tmp.c_str());
		err = "cannot protect AI provider health database";
		return false;
	}
#endif
	if (rename(tmp.c_str(), path.c_str()) != 0) {
		err = std::string(
			      "cannot install AI provider health database: ") +
		      strerror(errno);
		return false;
	}
	return true;
}

bool save_unlocked(const std::string &user_root,
		   const std::vector<provider_config_t> &providers,
		   std::string &err)
{
	const std::string dir = agent_dir(user_root);
	if (!ensure_private_dir(dir, err))
		return false;
	const std::string path = provider_file(user_root);
	const std::string tmp = path + ".tmp";
	std::ofstream out(tmp.c_str(),
			  std::ios::out | std::ios::trunc | std::ios::binary);
	if (!out.good()) {
		err = "cannot write AI provider database";
		return false;
	}
	out << kHeader << '\n';
	for (size_t i = 0; i < providers.size(); ++i) {
		const provider_config_t &item = providers[i];
		out << hex_encode(item.id) << '\t' << hex_encode(item.name)
		    << '\t' << hex_encode(item.protocol) << '\t'
		    << hex_encode(item.base_url) << '\t'
		    << hex_encode(item.model) << '\t'
		    << hex_encode(item.api_key_ciphertext) << '\t'
		    << hex_encode(item.api_key_hint) << '\t'
		    << (item.enabled ? "1" : "0") << '\t'
		    << (item.allow_file_content ? "1" : "0") << '\t'
		    << (item.is_default ? "1" : "0") << '\t'
		    << (item.responses_store ? "1" : "0") << '\t'
		    << (item.responses_background ? "1" : "0") << '\t'
		    << (item.responses_compact ? "1" : "0") << '\t'
		    << (item.responses_strict_tools ? "1" : "0") << '\t'
		    << hex_encode(item.responses_min_reasoning_effort) << '\t'
		    << hex_encode(item.responses_reasoning_summary) << '\t'
		    << hex_encode(item.responses_text_verbosity) << '\t'
		    << hex_encode(item.responses_service_tier) << '\t'
		    << hex_encode(item.responses_cache_ttl) << '\t'
		    << hex_encode(item.openai_organization) << '\t'
		    << hex_encode(item.openai_project) << '\t'
		    << (item.qwen_session_cache ? "1" : "0") << '\n';
	}
	out.close();
	if (!out.good()) {
		err = "cannot flush AI provider database";
		return false;
	}
#ifndef _WIN32
	if (chmod(tmp.c_str(), 0600) != 0) {
		unlink(tmp.c_str());
		err = "cannot protect AI provider database";
		return false;
	}
#endif
	if (rename(tmp.c_str(), path.c_str()) != 0) {
		err = std::string("cannot install AI provider database: ") +
		      strerror(errno);
		return false;
	}
	return true;
}

bool protocol_supported(const std::string &protocol)
{
	return protocol == "openai_images" || protocol == "openai_responses" ||
	       protocol == "openai_chat" || protocol == "openai_compatible" ||
	       protocol == "ollama" || protocol == "anthropic_messages" ||
	       protocol == "gemini_native";
}

bool parse_base_url(const std::string &url, bool &https, std::string &hostname,
		    std::string &err)
{
	const std::string lower = lower_ascii(url);
	size_t begin = 0;
	if (lower.compare(0, 8, "https://") == 0) {
		https = true;
		begin = 8;
	} else if (lower.compare(0, 7, "http://") == 0) {
		https = false;
		begin = 7;
	} else {
		err = "AI provider URL must use HTTP or HTTPS";
		return false;
	}
	const size_t slash = url.find('/', begin);
	std::string authority = url.substr(begin, slash == std::string::npos ?
							  std::string::npos :
							  slash - begin);
	if (authority.empty() || authority.find('@') != std::string::npos ||
	    authority.find_first_of(" \t\r\n?#") != std::string::npos) {
		err = "AI provider URL has an invalid host";
		return false;
	}
	if (authority[0] == '[') {
		const size_t close = authority.find(']');
		if (close == std::string::npos) {
			err = "AI provider URL has an invalid IPv6 host";
			return false;
		}
		hostname = lower_ascii(authority.substr(1, close - 1));
	} else {
		const size_t colon = authority.find(':');
		hostname = lower_ascii(authority.substr(0, colon));
	}
	if (hostname.empty()) {
		err = "AI provider URL has an invalid host";
		return false;
	}
	return true;
}

bool is_loopback_host(const std::string &host)
{
	return host == "localhost" || host == "127.0.0.1" || host == "::1";
}

} // namespace

provider_store_t::provider_store_t(const std::string &upload_root,
				   const std::string &user_root,
				   const std::string &username)
	: upload_root_(upload_root)
	, user_root_(user_root)
	, username_(username)
{
}

bool provider_store_t::list(std::vector<provider_config_t> &providers,
			    std::string &err) const
{
	std::lock_guard<webcool::mutex> guard(g_provider_store_mutex);
	if (!load_unlocked(user_root_, providers, err)) {
		return ai_error("provider.store", "list", err);
	}
	if (!load_health_unlocked(user_root_, providers, err)) {
		return ai_error("provider.store", "list-health", err);
	}
	return true;
}

bool provider_store_t::save(const provider_input_t &input,
			    provider_config_t &saved, std::string &err)
{
	std::lock_guard<webcool::mutex> guard(g_provider_store_mutex);
	std::vector<provider_config_t> providers;
	if (!load_unlocked(user_root_, providers, err)) {
		return ai_error("provider.store", "load-for-save", err);
	}
	provider_config_t *target = NULL;
	if (!input.id.empty()) {
		if (!valid_id(input.id)) {
			err = "invalid AI provider id";
			return ai_error("provider.store", "validate-save-id",
					err);
		}
		for (size_t i = 0; i < providers.size(); ++i) {
			if (providers[i].id == input.id)
				target = &providers[i];
		}
		if (target == NULL) {
			err = "AI provider not found";
			return ai_error("provider.store", "find-for-save", err);
		}
	} else {
		provider_config_t item;
		item.id = new_id();
		if (item.id.empty()) {
			err = "cannot generate AI provider id";
			return ai_error("provider.store", "generate-id", err);
		}
		providers.push_back(item);
		target = &providers.back();
	}
	target->name = input.name;
	target->protocol = input.protocol;
	target->base_url = input.base_url;
	target->model = input.model;
	target->enabled = input.enabled;
	target->allow_file_content = input.allow_file_content;
	target->is_default = input.is_default;
	target->responses_store = input.responses_store;
	target->responses_background = input.responses_background;
	target->responses_compact = input.responses_compact;
	target->responses_strict_tools = input.responses_strict_tools;
	target->responses_min_reasoning_effort =
		input.responses_min_reasoning_effort;
	target->responses_reasoning_summary = input.responses_reasoning_summary;
	target->responses_text_verbosity = input.responses_text_verbosity;
	target->responses_service_tier = input.responses_service_tier;
	target->responses_cache_ttl = input.responses_cache_ttl;
	target->qwen_session_cache = input.qwen_session_cache;
	target->openai_organization = input.openai_organization;
	target->openai_project = input.openai_project;
	if (input.clear_api_key) {
		target->api_key_ciphertext.clear();
		target->api_key_hint.clear();
	} else if (!input.api_key.empty()) {
		// Plaintext exists only for this call. Encryption happens before the
		// provider record is serialized, and no logging call receives the key.
		if (!encrypt_secret(upload_root_, username_ + ":" + target->id,
				    input.api_key, target->api_key_ciphertext,
				    err)) {
			return ai_error("provider.store", "encrypt-api-key",
					err);
		}
		const size_t hint_len =
			std::min<size_t>(4, input.api_key.size());
		target->api_key_hint =
			input.api_key.substr(input.api_key.size() - hint_len);
	}
	if (target->is_default) {
		for (size_t i = 0; i < providers.size(); ++i) {
			if (&providers[i] != target)
				providers[i].is_default = false;
		}
	}
	if (!save_unlocked(user_root_, providers, err)) {
		return ai_error("provider.store", "save", err);
	}
	saved = *target;
	return true;
}

bool provider_store_t::remove(const std::string &id, std::string &err)
{
	std::lock_guard<webcool::mutex> guard(g_provider_store_mutex);
	if (!valid_id(id)) {
		err = "invalid AI provider id";
		return ai_error("provider.store", "validate-remove-id", err);
	}
	std::vector<provider_config_t> providers;
	if (!load_unlocked(user_root_, providers, err)) {
		return ai_error("provider.store", "load-for-remove", err);
	}
	if (!load_health_unlocked(user_root_, providers, err)) {
		return ai_error("provider.store", "load-health-for-remove",
				err);
	}
	const size_t before = providers.size();
	providers.erase(std::remove_if(providers.begin(), providers.end(),
				       [&id](const provider_config_t &item) {
					       return item.id == id;
				       }),
			providers.end());
	if (providers.size() == before) {
		err = "AI provider not found";
		return ai_error("provider.store", "find-for-remove", err);
	}
	if (!save_unlocked(user_root_, providers, err)) {
		return ai_error("provider.store", "save-remove", err);
	}
	// Rewrite the small telemetry file as well so deleting a provider does not
	// leave its diagnostic history behind in the user's directory.
	if (!save_health_unlocked(user_root_, providers, err)) {
		return ai_error("provider.store", "save-health-remove", err);
	}
	return true;
}

bool provider_store_t::reveal_api_key(const provider_config_t &provider,
				      std::string &api_key,
				      std::string &err) const
{
	// Rotation changes the key journal and provider files as one logical unit.
	// Serialize decryptions with it so a request cannot observe a half-written
	// cross-file transition.
	std::lock_guard<webcool::mutex> guard(g_provider_store_mutex);
	std::vector<provider_config_t> providers;
	if (!load_unlocked(user_root_, providers, err)) {
		return ai_error("provider.store", "reload-for-decrypt", err);
	}
	const provider_config_t *current = NULL;
	for (size_t i = 0; i < providers.size(); ++i) {
		if (providers[i].id == provider.id) {
			current = &providers[i];
			break;
		}
	}
	if (current == NULL) {
		err = "AI provider not found";
		return ai_error("provider.store", "find-for-decrypt", err);
	}
	if (!decrypt_secret(upload_root_, username_ + ":" + current->id,
			    current->api_key_ciphertext, api_key, err)) {
		return ai_error("provider.store", "decrypt-api-key", err);
	}
	return true;
}

bool provider_store_t::rotate_master_key(const std::string &upload_root,
					 master_key_rotation_result_t &result,
					 std::string &err)
{
	std::lock_guard<webcool::mutex> guard(g_provider_store_mutex);
	result = master_key_rotation_result_t();
	master_keyring_t keyring;
	if (!read_master_keyring(upload_root, keyring, err)) {
		return ai_error("provider.master-key", "load", err);
	}
	result.recovered_interrupted_rotation = !keyring.phase.empty();
	// Enumerate before creating the dual-key journal. This avoids beginning a
	// new rotation when an encrypted agent checkpoint still depends on the
	// current active key. An already prepared journal remains readable and will
	// resume after the recoverable runs have been closed.
	std::vector<rotation_user_t> users;
	if (!collect_rotation_users(upload_root, users, err)) {
		return ai_error("provider.master-key", "enumerate-users", err);
	}

	std::vector<unsigned char> target_key;
	if (keyring.phase == "prepared") {
		target_key = keyring.secondary;
	} else if (keyring.phase == "committed") {
		target_key = keyring.active;
	} else {
		target_key.assign(32, 0);
		if (RAND_bytes(&target_key[0],
			       static_cast<int>(target_key.size())) != 1) {
			err = "cannot generate replacement AI master key";
			return ai_error("provider.master-key", "generate", err);
		}
		// Installing the dual-key journal before touching provider files makes
		// every subsequent mixed state readable and safely resumable.
		if (!write_master_key_file(upload_root, keyring.active,
					   target_key, "prepared", err)) {
			return ai_error("provider.master-key",
					"prepare-journal", err);
		}
		keyring.secondary = target_key;
		keyring.phase = "prepared";
	}
	cleanse_vector_t target_guard(target_key);

	for (size_t i = 0; i < users.size(); ++i) {
		std::vector<provider_config_t> providers;
		if (!load_unlocked(users[i].user_root, providers, err)) {
			return ai_error("provider.master-key",
					"load-provider-file", err);
		}
		++result.users_scanned;
		bool changed = false;
		for (size_t j = 0; j < providers.size(); ++j) {
			provider_config_t &provider = providers[j];
			if (provider.api_key_ciphertext.empty())
				continue;
			std::string plaintext;
			std::string decrypt_err;
			const std::string aad =
				users[i].username + ":" + provider.id;
			bool decrypted = decrypt_secret_with_key(
				keyring.active, aad,
				provider.api_key_ciphertext, plaintext,
				decrypt_err);
			if (!decrypted && !keyring.secondary.empty()) {
				decrypted = decrypt_secret_with_key(
					keyring.secondary, aad,
					provider.api_key_ciphertext, plaintext,
					decrypt_err);
			}
			if (!decrypted) {
				if (!plaintext.empty())
					OPENSSL_cleanse(&plaintext[0],
							plaintext.size());
				err = "cannot decrypt an AI provider during master key rotation";
				return ai_error("provider.master-key",
						"decrypt-provider", err);
			}
			std::string rotated;
			const bool encrypted = encrypt_secret_with_key(
				target_key, aad, plaintext, rotated, err);
			if (!plaintext.empty())
				OPENSSL_cleanse(&plaintext[0],
						plaintext.size());
			if (!encrypted) {
				return ai_error("provider.master-key",
						"reencrypt-provider", err);
			}
			provider.api_key_ciphertext.swap(rotated);
			changed = true;
			++result.providers_reencrypted;
		}
		if (changed &&
		    !save_unlocked(users[i].user_root, providers, err)) {
			return ai_error("provider.master-key",
					"save-provider-file", err);
		}
	}

	if (keyring.phase == "prepared") {
		// All provider files now use target_key. Mark it active while retaining
		// the old key until the explicit verification pass below completes.
		if (!write_master_key_file(upload_root, target_key,
					   keyring.active, "committed", err)) {
			return ai_error("provider.master-key", "commit-journal",
					err);
		}
	}

	for (size_t i = 0; i < users.size(); ++i) {
		std::vector<provider_config_t> providers;
		if (!load_unlocked(users[i].user_root, providers, err)) {
			return ai_error("provider.master-key", "verify-load",
					err);
		}
		for (size_t j = 0; j < providers.size(); ++j) {
			if (providers[j].api_key_ciphertext.empty())
				continue;
			std::string plaintext;
			const std::string aad =
				users[i].username + ":" + providers[j].id;
			const bool verified = decrypt_secret_with_key(
				target_key, aad,
				providers[j].api_key_ciphertext, plaintext,
				err);
			if (!plaintext.empty())
				OPENSSL_cleanse(&plaintext[0],
						plaintext.size());
			if (!verified) {
				err = "AI provider verification failed after master key rotation";
				return ai_error("provider.master-key",
						"verify-provider", err);
			}
		}
	}

	// Only after every ciphertext verifies under the replacement key do we
	// remove the old key from disk and finish the rotation.
	std::vector<unsigned char> no_secondary;
	if (!write_master_key_file(upload_root, target_key, no_secondary, "",
				   err)) {
		return ai_error("provider.master-key", "retire-old-key", err);
	}
	return true;
}

bool provider_store_t::seal_user_data(const std::string &upload_root,
				      const std::string &username,
				      const std::string &binding,
				      const std::string &plaintext,
				      std::string &encoded, std::string &err)
{
	std::lock_guard<webcool::mutex> guard(g_provider_store_mutex);
	if (username.empty() || binding.empty()) {
		err = "invalid AI user data encryption binding";
		return ai_error("provider.user-data", "validate-seal", err);
	}
	if (!encrypt_secret(upload_root,
			    "user-data:" + username + ":" + binding, plaintext,
			    encoded, err)) {
		return ai_error("provider.user-data", "seal", err);
	}
	return true;
}

bool provider_store_t::open_user_data(const std::string &upload_root,
				      const std::string &username,
				      const std::string &binding,
				      const std::string &encoded,
				      std::string &plaintext, std::string &err)
{
	std::lock_guard<webcool::mutex> guard(g_provider_store_mutex);
	if (username.empty() || binding.empty()) {
		err = "invalid AI user data encryption binding";
		return ai_error("provider.user-data", "validate-open", err);
	}
	if (!decrypt_secret(upload_root,
			    "user-data:" + username + ":" + binding, encoded,
			    plaintext, err)) {
		return ai_error("provider.user-data", "open", err);
	}
	return true;
}

bool provider_store_t::record_test_result(const std::string &id, bool succeeded,
					  int http_status, long long latency_ms,
					  const std::string &error_summary,
					  std::string &err)
{
	std::lock_guard<webcool::mutex> guard(g_provider_store_mutex);
	if (!valid_id(id)) {
		err = "invalid AI provider id";
		return ai_error("provider.store", "validate-test-result-id",
				err);
	}
	std::vector<provider_config_t> providers;
	if (!load_unlocked(user_root_, providers, err)) {
		return ai_error("provider.store", "load-for-test-result", err);
	}
	if (!load_health_unlocked(user_root_, providers, err)) {
		return ai_error("provider.store", "load-health-for-test-result",
				err);
	}
	provider_config_t *target = NULL;
	for (size_t i = 0; i < providers.size(); ++i) {
		if (providers[i].id == id) {
			target = &providers[i];
			break;
		}
	}
	if (target == NULL) {
		err = "AI provider not found";
		return ai_error("provider.store", "find-for-test-result", err);
	}
	target->last_test_status = succeeded ? "ok" : "error";
	target->last_test_at = static_cast<long long>(std::time(NULL));
	target->last_test_http_status = http_status < 0 ? 0 : http_status;
	target->last_test_latency_ms = latency_ms < 0 ? 0 : latency_ms;
	target->last_test_error =
		succeeded ? "" : sanitize_test_error(error_summary);
	if (!save_health_unlocked(user_root_, providers, err)) {
		return ai_error("provider.store", "save-test-result", err);
	}
	return true;
}

bool provider_store_t::validate_input(const provider_input_t &input,
				      bool allow_insecure_remote,
				      std::string &err)
{
	if (input.name.empty() || input.name.size() > 80) {
		err = "AI provider name is required and must not exceed 80 characters";
		return ai_error("provider.store", "validate-name", err);
	}
	if (!protocol_supported(input.protocol)) {
		err = "unsupported AI provider protocol";
		return ai_error("provider.store", "validate-protocol", err);
	}
	if (input.base_url.empty() || input.base_url.size() > 2048) {
		err = "AI provider URL is required";
		return ai_error("provider.store", "validate-url-length", err);
	}
	if (input.base_url.find('?') != std::string::npos ||
	    input.base_url.find('#') != std::string::npos) {
		err = "AI provider URL must not contain a query or fragment";
		return ai_error("provider.store", "validate-url-components",
				err);
	}
	if (input.model.empty() || input.model.size() > 200) {
		err = "AI model name is required";
		return ai_error("provider.store", "validate-model", err);
	}
	const bool valid_min_effort =
		input.responses_min_reasoning_effort == "auto" ||
		input.responses_min_reasoning_effort == "none" ||
		input.responses_min_reasoning_effort == "low";
	const bool valid_summary =
		input.responses_reasoning_summary == "auto" ||
		input.responses_reasoning_summary == "concise" ||
		input.responses_reasoning_summary == "detailed" ||
		input.responses_reasoning_summary == "none";
	const bool valid_verbosity =
		input.responses_text_verbosity == "low" ||
		input.responses_text_verbosity == "medium" ||
		input.responses_text_verbosity == "high";
	const bool valid_tier = input.responses_service_tier == "auto" ||
				input.responses_service_tier == "default" ||
				input.responses_service_tier == "flex" ||
				input.responses_service_tier == "priority" ||
				input.responses_service_tier == "fast" ||
				input.responses_service_tier == "ultrafast";
	if (!valid_min_effort || !valid_summary || !valid_verbosity ||
	    !valid_tier ||
	    (input.responses_cache_ttl != "30m" &&
	     input.responses_cache_ttl != "none")) {
		err = "invalid OpenAI Responses advanced setting";
		return ai_error("provider.store", "validate-responses-settings",
				err);
	}
	if (input.protocol == "openai_responses" &&
	    input.responses_background && !input.responses_store) {
		err = "OpenAI background responses require stored response state";
		return ai_error("provider.store", "validate-background-store",
				err);
	}
	if (input.openai_organization.size() > 200 ||
	    input.openai_project.size() > 200 ||
	    input.openai_organization.find_first_of("\r\n") !=
		    std::string::npos ||
	    input.openai_project.find_first_of("\r\n") != std::string::npos) {
		err = "invalid OpenAI organization or project header";
		return ai_error("provider.store", "validate-openai-routing",
				err);
	}
	if (input.api_key.size() > 8192) {
		err = "AI API key is too long";
		return ai_error("provider.store", "validate-api-key-length",
				err);
	}
	bool https = false;
	std::string hostname;
	if (!parse_base_url(input.base_url, https, hostname, err)) {
		return ai_error("provider.store", "parse-base-url", err);
	}
	if (!https && !is_loopback_host(hostname) && !allow_insecure_remote) {
		err = "non-local AI providers must use HTTPS";
		return ai_error("provider.store", "require-https", err);
	}
	const std::string lower_model = lower_ascii(input.model);
	const std::string lower_url = lower_ascii(input.base_url);
	const char *maas_suffix = ".maas.aliyuncs.com";
	const size_t maas_suffix_size = strlen(maas_suffix);
	const bool maas_host =
		hostname.size() > maas_suffix_size &&
		hostname.compare(hostname.size() - maas_suffix_size,
				 maas_suffix_size, maas_suffix) == 0;
	const bool official_qwen_url =
		lower_url.find("/compatible-mode/v1") != std::string::npos &&
		(hostname == "dashscope.aliyuncs.com" ||
		 hostname == "dashscope-intl.aliyuncs.com" || maas_host);
	const bool third_party_model =
		lower_model.compare(0, 8, "deepseek") == 0 ||
		lower_model.compare(0, 4, "kimi") == 0;
	const bool qwen_responses =
		input.protocol == "openai_responses" && !third_party_model &&
		(lower_model.compare(0, 4, "qwen") == 0 || official_qwen_url);
	if (qwen_responses &&
	    (!input.responses_store || input.responses_background ||
	     input.responses_compact || !input.openai_organization.empty() ||
	     !input.openai_project.empty())) {
		err = "Qwen Responses requires stored state and does not support background, native compaction, or OpenAI routing headers";
		return ai_error("provider.store", "validate-qwen-responses",
				err);
	}
	if (input.qwen_session_cache && !qwen_responses) {
		err = "Qwen session cache is only available for Qwen Responses providers";
		return ai_error("provider.store", "validate-qwen-session-cache",
				err);
	}
	if (input.protocol == "ollama" && !is_loopback_host(hostname) &&
	    !https && !allow_insecure_remote) {
		err = "remote Ollama endpoints must use HTTPS";
		return ai_error("provider.store", "require-ollama-https", err);
	}
	return true;
}

} // namespace ai
} // namespace webcool
