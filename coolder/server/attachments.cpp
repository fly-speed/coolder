#include "stdafx.h"
#include "attachments.h"
#include "action/action_util.h"
#include "libai/runtime/coding_runtime.h"
#include "libai/common/identifiers.h"
#include <openssl/evp.h>
#include <filesystem>
#include <fstream>

namespace coolder
{
namespace
{
namespace fs = std::filesystem;
constexpr size_t max_file = 8 * 1024 * 1024;
constexpr size_t max_total = 16 * 1024 * 1024;
std::string text(acl::json_node *n)
{
	return n && n->is_string() ? n->get_string() : "";
}
bool fail(response_t &res, int status, const char *message)
{
	acl::json json;
	auto &root = json.create_node();
	root.add_text("error", message);
	return action::sendJson(res, status, root, false);
}
bool decode(const std::string &encoded, std::string &bytes)
{
	if (encoded.empty() || encoded.size() % 4 ||
	    encoded.size() > (max_file + 2) / 3 * 4) {
		return false;
	}
	size_t padding = encoded.back() == '=' ? 1 : 0;
	if (padding && encoded[encoded.size() - 2] == '=') {
		++padding;
	}
	if (encoded.find_first_not_of(
	        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/",
	        0) < encoded.size() - padding) {
		return false;
	}
	bytes.resize(encoded.size() / 4 * 3);
	int n = EVP_DecodeBlock(reinterpret_cast<unsigned char *>(&bytes[0]),
	    reinterpret_cast<const unsigned char *>(encoded.data()),
	    encoded.size());
	if (n < static_cast<int>(padding)) {
		return false;
	}
	bytes.resize(n - padding);
	return !bytes.empty() && bytes.size() <= max_file;
}
bool safe_name(const std::string &name)
{
	if (name.empty() || name.size() > 180 || name == "." || name == "..") {
		return false;
	}
	for (unsigned char c : name) {
		if (!(c < 32 || c == 127 ||
		        std::string("/\\<>\"&:").find(c) != std::string::npos))
			continue;
		return false;
	}
	return true;
}
// Every component is server-chosen, and existing symlinks are never followed.
fs::path attachment_root(const account_t &account)
{
	fs::path root = account_workspace(account);
	for (const auto *name : { ".webcool_agent", "attachments" }) {
		root /= name;
		if (fs::is_symlink(fs::symlink_status(root))) {
			throw std::runtime_error("unsafe attachment directory");
		}
		fs::create_directory(root);
		if (fs::is_directory(root))
			continue;
		throw std::runtime_error("invalid attachment directory");
	}
	return root;
}
struct draft_guard {
	fs::path path;
	bool keep = false;
	~draft_guard()
	{
		if (!keep) {
			std::error_code err;
			fs::remove_all(path, err);
		}
	}
};
}

bool attachment_route(
    request_t &req, response_t &res, const account_t &account, bool discard)
{
	using namespace action::agent_detail;
	auto *body = req.getJson(discard ? 4096 : 24 * 1024 * 1024);
	if (!body) {
		return fail(res, 400, "invalid attachment request");
	}
	const auto root = attachment_root(account);
	if (discard) {
		const auto draft = text((*body)["attachment_draft"]);
		if (draft.size() !=
		        std::string(".webcool_agent/attachments/draft-")
		                .size() +
		            24 ||
		    draft.compare(0,
		        std::string(".webcool_agent/attachments/draft-").size(),
		        ".webcool_agent/attachments/draft-") != 0 ||
		    draft.substr(draft.size() - 24)
		            .find_first_not_of("0123456789abcdef") !=
		        std::string::npos) {
			return fail(res, 400, "invalid attachment draft");
		}
		const auto path = root / fs::path(draft).filename();
		if (fs::is_symlink(fs::symlink_status(path))) {
			return fail(res, 400, "invalid attachment draft");
		}
		remove_temporary_attachment_draft(
		    draft, account_workspace(account));
		return reply(res, 200, "{\"ok\":true}");
	}
	auto *files = json_array_node((*body)["files"]);
	if (!files || !files->first_child()) {
		return fail(res, 400, "attachments required");
	}
	const auto id = webcool::ai::identifiers::new_id();
	if (id.empty()) {
		return fail(res, 500, "cannot create attachment draft");
	}
	const std::string draft =
	    ".webcool_agent/attachments/draft-" + id.substr(0, 24);
	const auto path = root / fs::path(draft).filename();
	if (!fs::create_directory(path)) {
		return fail(res, 500, "cannot create attachment draft");
	}
	draft_guard cleanup{ path };
	auto reject = [&](int status, const char *message) {
		std::error_code error;
		fs::remove_all(path, error);
		cleanup.keep = !error;
		return fail(res, status, message);
	};
	fs::permissions(path, fs::perms::owner_all);
	acl::json json;
	auto &result = json.create_node();
	auto &paths = json.create_array();
	result.add_child("attachments", paths);
	result.add_text("attachment_draft", draft.c_str());
	size_t count = 0, total = 0;
	for (auto *file = files->first_child(); file;
	     file = files->next_child()) {
		if (++count > 8) {
			return reject(400, "at most 8 attachments are allowed");
		}
		const auto name = text((*file)["name"]);
		if (!safe_name(name)) {
			return reject(400, "invalid attachment filename");
		}
		std::string data;
		if (!decode(text((*file)["base64"]), data)) {
			return reject(
			    400, "invalid attachment or file exceeds 8 MiB");
		}
		total += data.size();
		if (total > max_total) {
			return reject(400, "attachments exceed 16 MiB");
		}
		const auto filename = std::to_string(count) + "-" + name;
		std::ofstream output(path / filename, std::ios::binary);
		output.write(data.data(), data.size());
		output.close();
		if (!output) {
			return reject(500, "cannot save attachment");
		}
		paths.add_child(
		    json.create_array_text((draft + "/" + filename).c_str()));
	}
	std::vector<webcool::ai::completion_image_t> images;
	std::string context, err;
	if (!load_temporary_attachments(&paths, draft,
	        account_workspace(account), images, context, err)) {
		return reject(400, err.c_str());
	}
	cleanup.keep = true;
	return action::sendJson(res, 200, result, false);
}
}
