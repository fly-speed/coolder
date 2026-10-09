#pragma once

#include <string>
#include <vector>

namespace webcool
{
namespace ai
{

// Provider-independent tool request produced by either native function calling
// or the fallback JSON protocol. Paths are relative to the authenticated user's
// root and are checked again to remain inside the selected project.
struct agent_tool_request_t {
	// Name used to identify this item in its containing collection.
	std::string name;
	// Workspace-relative path addressed by this operation.
	std::string path;
	// Search expression or query supplied to the tool.
	std::string query;
	// Fine-grained edits use an exact, uniquely matching old fragment. This is
	// deliberately text based instead of line-number based so edits remain safe
	// when nearby lines move between model turns.
	std::string old_text;
	// Destination path for a move or related structural change.
	std::string target_path;
	// Used by workspace.create/workspace.propose. The latter only creates a
	// review proposal and never overwrites the formal project file directly.
	std::string content;
	// Whether the request explicitly supplied a content string.
	bool content_present =
	    false; // Explicit JSON string, including an intentional empty deletion.
};

// A model proposal remains outside the formal project until server-side
// validation builds an atomic preview and the authenticated user accepts it.
// Draft build/test execution uses a private materialized copy of these bytes.
struct agent_change_proposal_t {
	// Requested change operation, such as write, delete or move.
	std::string operation;
	// Workspace-relative path addressed by this operation.
	std::string path;
	// Destination path for a move or related structural change.
	std::string target_path;
	// Text payload associated with this operation.
	std::string content;
	// Explanation supplied for this change or decision.
	std::string reason;
	// Filled by server-side workspace validation, never trusted from the model.
	bool creates_file = false;
	// Whether applying the proposal creates a directory.
	bool creates_directory = false;
	// Filled from the trusted server-side transaction preview immediately before
	// files are applied. Keeping the diff lets the workbench show deleted lines
	// after the new file contents have already been committed.
	long long added_lines = 0;
	// Number of source lines removed by the proposed edit.
	long long removed_lines = 0;
	// Bounded textual diff used for review.
	std::string diff;
	// Trusted pre-transaction baseline used to resolve per-line review choices
	// after the generated version has already been saved automatically.
	std::string original_content;
	// Whether a captured baseline is available for comparison.
	bool original_content_available = false;
	// Review identity is assigned by the server after workspace validation.
	// A later edit of the same path receives a new generation, so accepting an
	// older revision can never suppress a genuinely newer proposal.
	unsigned long long generation = 0;
	// Digest of the source version on which the edit was based.
	std::string base_hash;
	// Digest identifying this exact proposed content revision.
	std::string draft_hash;
	// pending, accepted or rejected. Only pending generations are presented as
	// review work; accepted/rejected generations remain in the durable artifact
	// as an audit-friendly record of the user's decision.
	std::string review_status = "pending";
};

// One decoded model turn. A turn is either a final message or a tool request;
// final messages may additionally carry bounded file-change proposals.
struct agent_protocol_message_t {
	// Whether the parsed protocol message ends the model turn.
	bool final_message = false;
	// User-facing answer extracted from the final protocol message.
	std::string final_text;
	// Optional bounded memory for a user-enabled multi-turn session. It should
	// describe goals, decisions and next steps without copying source code.
	std::string memory_summary;
	// Concise, user-facing record of what this individual run accomplished.
	// Unlike memory_summary, this is displayed with the corresponding turn and
	// is not used as cumulative model context.
	std::string completion_summary;
	// Serialized progress evidence for the task requirements.
	std::string requirement_progress_json;
	// Short human-readable label for the conversation selector (10-20 chars).
	std::string session_title;
	// Normalized tool request selected for execution.
	agent_tool_request_t tool;
	// File proposals associated with this result or running task.
	std::vector<agent_change_proposal_t> changes;
};

// Returns false when the model did not emit the structured WebCool protocol.
// Callers may safely treat that output as a plain final response.
bool parse_agent_protocol_message(
    const std::string &raw, agent_protocol_message_t &message);

} // namespace ai
} // namespace webcool
