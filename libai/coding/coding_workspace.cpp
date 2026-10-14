#include "coding_internal.h"
#include "../workspace/agent_workspace.h"
#include "../workspace/workspace_patch.h"
#include "../workspace/workspace_change_set.h"
namespace libai
{
namespace ai
{
namespace coding_detail
{
namespace
{
bool patches(const std::string &root, const std::string &op, acl::json_node &a,
    acl::json_node &out, std::string &error)
{
	workspace_patch_store_t store(root);
	if (op == "patch.preview") {
		workspace_patch_preview_t p;
		if (!store.create(
		        text(a, "path"), text(a, "content"), p, error))
			return false;
		out.add_text("id", p.id.c_str())
		    .add_text("path", p.path.c_str())
		    .add_text("original_sha256", p.original_sha256.c_str())
		    .add_text("proposed_sha256", p.proposed_sha256.c_str())
		    .add_text("diff", p.diff.c_str())
		    .add_number("added_lines", p.added_lines)
		    .add_number("removed_lines", p.removed_lines);
		return true;
	}
	if (op == "patch.apply") {
		workspace_patch_result_t p;
		if (!store.apply(text(a, "id"), p, error))
			return false;
		out.add_text("id", p.id.c_str())
		    .add_text("path", p.path.c_str())
		    .add_text("sha256", p.sha256.c_str());
		return true;
	}
	return false;
}
bool changes(const std::string &root, const std::string &op, acl::json_node &a,
    acl::json_node &out, std::string &error)
{
	workspace_change_set_store_t store(root);
	if (op == "changes.recover")
		return store.recover(error);
	if (op == "changes.preview") {
		auto *items = array_value(a["changes"]);
		if (!items || !items->is_array())
			throw std::invalid_argument("changes must be an array");
		std::vector<workspace_change_input_t> list;
		for (auto *n = items->first_child(); n;
		     n = items->next_child()) {
			if (!n->is_object() || list.size() >= 100)
				throw std::invalid_argument(
				    "invalid or excessive changes");
			workspace_change_input_t item;
			item.operation = text(*n, "operation");
			item.path = text(*n, "path");
			item.target_path = text(*n, "target_path");
			item.content = text(*n, "content");
			item.reason = text(*n, "reason");
			list.push_back(item);
		}
		workspace_change_set_preview_t preview;
		if (!store.create(list, preview, error))
			return false;
		out.add_text("id", preview.id.c_str());
		auto &array = coding_detail::array(out, "items");
		for (const auto &p : preview.items) {
			auto &n = array.add_child(false, true);
			n.add_text("operation", p.operation.c_str())
			    .add_text("path", p.path.c_str())
			    .add_text("target_path", p.target_path.c_str())
			    .add_text("diff", p.diff.c_str())
			    .add_text(
			        "original_sha256", p.original_sha256.c_str())
			    .add_text(
			        "proposed_sha256", p.proposed_sha256.c_str())
			    .add_bool("creates_file", p.creates_file)
			    .add_bool("deletes_file", p.deletes_file)
			    .add_bool("creates_directory", p.creates_directory);
		}
		return true;
	}
	if (op == "changes.apply") {
		std::vector<workspace_change_result_t> results;
		if (!store.apply(text(a, "id"), results, error))
			return false;
		auto &array = coding_detail::array(out, "items");
		for (const auto &p : results) {
			array.add_child(false, true)
			    .add_text("operation", p.operation.c_str())
			    .add_text("path", p.path.c_str())
			    .add_text("target_path", p.target_path.c_str())
			    .add_text("sha256", p.sha256.c_str())
			    .add_bool("created", p.created)
			    .add_bool("deleted", p.deleted);
		}
		return true;
	}
	return false;
}
}
bool workspace(const std::string &root, const std::string &op,
    acl::json_node &a, acl::json_node &out, std::string &error)
{
	if (op.compare(0, 6, "patch.") == 0)
		return patches(root, op, a, out, error);
	if (op.compare(0, 8, "changes.") == 0)
		return changes(root, op, a, out, error);
	agent_workspace_t w(root, false);
	const auto path = text(a, "path");
	if (op == "workspace.create")
		return w.create_text_if_absent(path, text(a, "content"), error);
	if (op == "workspace.replace")
		return w.replace_text_if_unchanged(path,
		    text(a, "expected_sha256"), text(a, "content"), error);
	if (op == "workspace.delete")
		return w.delete_text_if_unchanged(
		    path, text(a, "expected_sha256"), error);
	if (op == "workspace.mkdir")
		return w.create_directory_if_absent(path, error);
	if (op == "workspace.rmdir")
		return w.delete_empty_directory(path, error);
	if (op == "workspace.sha256") {
		std::string digest;
		if (!w.file_sha256(path, digest, error))
			return false;
		out.add_text("sha256", digest.c_str());
		return true;
	}
	if (op == "workspace.read") {
		std::string content;
		bool truncated = false;
		if (!w.read(path, content, truncated, error))
			return false;
		out.add_text("content", content.c_str())
		    .add_bool("truncated", truncated);
		return true;
	}
	if (op == "workspace.list") {
		std::vector<workspace_entry_t> entries;
		if (!w.list(path, entries, error))
			return false;
		auto &array = coding_detail::array(out, "entries");
		for (const auto &p : entries)
			array.add_child(false, true)
			    .add_text("path", p.path.c_str())
			    .add_bool("directory", p.directory)
			    .add_number("size", p.size)
			    .add_number("modified_at", p.modified_at);
		return true;
	}
	if (op == "workspace.search") {
		std::vector<workspace_match_t> entries;
		bool truncated = false;
		if (!w.search(
		        path, text(a, "query"), entries, truncated, error))
			return false;
		auto &array = coding_detail::array(out, "matches");
		for (const auto &p : entries)
			array.add_child(false, true)
			    .add_text("path", p.path.c_str())
			    .add_number("line", p.line)
			    .add_text("text", p.text.c_str());
		out.add_bool("truncated", truncated);
		return true;
	}
	if (op == "workspace.outline") {
		std::vector<workspace_outline_item_t> entries;
		bool truncated = false;
		if (!w.outline(path, entries, truncated, error))
			return false;
		auto &array = coding_detail::array(out, "items");
		for (const auto &p : entries)
			array.add_child(false, true)
			    .add_number("line", p.line)
			    .add_text("kind", p.kind.c_str())
			    .add_text("text", p.text.c_str());
		out.add_bool("truncated", truncated);
		return true;
	}
	return false;
}
}
}
}
