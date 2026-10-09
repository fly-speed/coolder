#pragma once

#include <string>
#include <vector>

namespace acl
{
class json;
class json_node;
}

namespace webcool
{
namespace ai
{

struct assistant_message_t {
	std::string role;
	std::string text;
	std::string format;
	std::string run_id;
	bool failed = false;
	long long input = -1;
	long long output = -1;
	long long duration = 0;
	long long created_at = 0;
};

struct assistant_session_t {
	std::string id;
	std::string title;
	long long updated = 0;
	bool pinned = false;
	std::vector<std::string> tag_ids;
	std::vector<assistant_message_t> messages;
};

bool assistant_session_id_valid(const std::string &id);
bool assistant_session_parse(acl::json_node *node, assistant_session_t &session,
			     std::string &err);
void assistant_session_json(acl::json &json, acl::json_node &node,
			    const assistant_session_t &session, bool detail);

class assistant_session_store_t {
public:
	explicit assistant_session_store_t(const std::string &user_root);

	bool list(std::vector<assistant_session_t> &sessions,
		  std::string &err) const;
	bool get(const std::string &id, assistant_session_t &session,
		 std::string &err) const;
	// Migration is create-only: retrying must never overwrite server messages.
	bool import_session(const assistant_session_t &session,
			    std::string &err) const;
	bool append(const std::string &id, const assistant_message_t &message,
		    std::string &err) const;

	bool update(const std::string &operation,
		    const std::vector<std::string> &ids,
		    const std::string &title, std::string &err) const;

	bool save_image(const std::string &id, const std::string &session_id,
			const std::string &mime, const std::string &bytes,
			std::string &err) const;
	bool load_image(const std::string &id, std::string &mime,
			std::string &bytes, std::string &err) const;
	bool add_tag(const std::string &id, const std::string &tag_id,
		     std::string &err) const;
	bool list_tag(const std::string &tag_id,
		      std::vector<assistant_session_t> &sessions,
		      std::string &err) const;
	bool remove_tags(const std::vector<std::string> &tag_ids,
			 std::string &err) const;

private:
	std::string user_root_;
};

} // namespace ai
} // namespace webcool
