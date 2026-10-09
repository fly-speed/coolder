#pragma once

#include <string>
#include <vector>

namespace acl
{
// JSON document type supplied by the ACL library.
class json;
// JSON node type supplied by the ACL library.
class json_node;
}

namespace webcool
{
namespace ai
{

// One saved message in an assistant conversation.
struct assistant_message_t {
	// Conversation role, such as user or assistant.
	std::string role;
	// Text content presented or retained by this record.
	std::string text;
	// Format identifier of the serialized record or generated artifact.
	std::string format;
	// Identifier of the associated agent run.
	std::string run_id;
	// Whether the tracked operation ended in failure.
	bool failed = false;
	// Input token count, or -1 when usage is unknown.
	long long input = -1;
	// Output token count, or -1 when usage is unknown.
	long long output = -1;
	// Recorded request duration in milliseconds.
	long long duration = 0;
	// Message creation time as milliseconds since the Unix epoch.
	long long created_at = 0;
};

// Saved assistant conversation and its user-managed metadata.
struct assistant_session_t {
	// Identifier used to look up this record.
	std::string id;
	// User-visible title of this record.
	std::string title;
	// Last session update time as milliseconds since the Unix epoch.
	long long updated = 0;
	// Whether the session is pinned in the conversation list.
	bool pinned = false;
	// Tags assigned to the assistant conversation.
	std::vector<std::string> tag_ids;
	// Ordered messages belonging to this conversation.
	std::vector<assistant_message_t> messages;
};

// Validate the identifier used to address an assistant session.
bool assistant_session_id_valid(const std::string &id);
// Parse and validate a saved assistant conversation from JSON.
bool assistant_session_parse(
    acl::json_node *node, assistant_session_t &session, std::string &err);
// Serialize assistant conversation metadata and optional message details.
void assistant_session_json(acl::json &json, acl::json_node &node,
    const assistant_session_t &session, bool detail);

// Persists and retrieves assistant session records within the configured
// storage scope.
class assistant_session_store_t {
public:
	// Bind the assistant session store to the supplied storage scope.
	explicit assistant_session_store_t(const std::string &user_root);

	// Return the saved conversations ordered by pinning and recency.
	bool list(
	    std::vector<assistant_session_t> &sessions, std::string &err) const;
	// Read the record identified by the supplied key; report failures
	// through err.
	bool get(const std::string &id, assistant_session_t &session,
	    std::string &err) const;
	// Migration is create-only: retrying must never overwrite server messages.
	bool import_session(
	    const assistant_session_t &session, std::string &err) const;
	// Append one bounded message to the identified saved conversation.
	bool append(const std::string &id, const assistant_message_t &message,
	    std::string &err) const;

	// Update the identified persistent record; report failures through
	// err.
	bool update(const std::string &operation,
	    const std::vector<std::string> &ids, const std::string &title,
	    std::string &err) const;

	// Persist the generated image and return its attachment metadata.
	bool save_image(const std::string &id, const std::string &session_id,
	    const std::string &mime, const std::string &bytes,
	    std::string &err) const;
	// Read an authorized image into the bounded in-memory request
	// payload.
	bool load_image(const std::string &id, std::string &mime,
	    std::string &bytes, std::string &err) const;
	// Associate the supplied tag with the identified assistant
	// conversation.
	bool add_tag(const std::string &id, const std::string &tag_id,
	    std::string &err) const;
	// Return assistant conversations associated with the requested tag.
	bool list_tag(const std::string &tag_id,
	    std::vector<assistant_session_t> &sessions, std::string &err) const;
	// Remove the supplied tags from stored assistant conversations.
	bool remove_tags(
	    const std::vector<std::string> &tag_ids, std::string &err) const;

private:
	// Filesystem root belonging to the authenticated user.
	std::string user_root_;
};

} // namespace ai
} // namespace webcool
