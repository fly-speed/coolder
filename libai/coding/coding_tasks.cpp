#include "coding_internal.h"
#include "task_contract_json.h"
#include "../workspace/agent_workspace.h"
#include <map>
#include <set>

namespace webcool { namespace ai { namespace coding_detail {
namespace {
acl::json_node *object(acl::json_node *n)
{
    if (n && !n->is_object()) n = n->get_obj();
    if (!n || !n->is_object()) throw std::invalid_argument("task record must be an object");
    return n;
}
bool excluded(const std::string &path)
{
    const std::set<std::string> names = {".git", ".webcool_agent", ".webcool-build",
        "build", "target", "dist", "node_modules", "var", ".env", "secrets", "private"};
    size_t begin = 0;
    while (begin < path.size()) {
        const auto end = path.find('/', begin);
        if (names.count(path.substr(begin, end - begin))) return true;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return false;
}
struct inventory {
    agent_workspace_t workspace;
    std::map<std::string, std::string> files;
    size_t entries = 0;
    long long bytes = 0;
    explicit inventory(const std::string &root) : workspace(root) {}
    void scan(const std::string &path, unsigned depth)
    {
        if (depth > 24) throw std::invalid_argument("task fingerprint exceeds depth limit");
        std::vector<workspace_entry_t> children; std::string error;
        if (!workspace.list(path, children, error)) throw std::runtime_error(error);
        // Listings are bounded by the workspace API; never trust a truncated inventory.
        if (children.size() >= 512) throw std::invalid_argument("task fingerprint directory too large");
        for (const auto &entry : children) {
            if (excluded(entry.path)) continue;
            if (++entries > 10000) throw std::invalid_argument("task fingerprint exceeds entry limit");
            if (entry.directory) {
                files[entry.path + "/"] = "";
                scan(entry.path, depth + 1);
            } else {
                bytes += entry.size;
                if (bytes > 128 * 1024 * 1024) throw std::invalid_argument("task fingerprint exceeds byte limit");
                std::string digest;
                if (!workspace.file_sha256(entry.path, digest, error)) throw std::runtime_error(error);
                files[entry.path] = digest;
            }
        }
    }
    void parents(const std::string &path)
    {
        size_t at = path.find('/');
        while (at != std::string::npos) {
            files[path.substr(0, at + 1)] = "";
            at = path.find('/', at + 1);
        }
    }
    void overlay(acl::json_node &c)
    {
        const auto path = text(c, "relative_path");
        std::string normalized, error;
        if (!agent_workspace_t::normalize_path(path, normalized, false, error) || excluded(path))
            throw std::invalid_argument("invalid task proposal path");
        const auto review = text(c, "review_status"), op = text(c, "operation");
        if (review == "accepted" || review == "rejected") return;
        if (!review.empty() && review != "pending") throw std::invalid_argument("invalid proposal review state");
        if (op == "write") {
            files[path] = agent_workspace_t::content_sha256(text(c, "content"));
            parents(path);
        } else if (op == "mkdir") {
            files[path + "/"] = ""; parents(path);
        } else if (op == "delete") {
            for (auto i = files.begin(); i != files.end();) {
                if (i->first == path || i->first.compare(0, path.size()+1, path+"/") == 0)
                    i = files.erase(i);
                else ++i;
            }
        } else if (op == "move") {
            const auto target = text(c, "target_path");
            if (!agent_workspace_t::normalize_path(target, normalized, false, error) || excluded(target))
                throw std::invalid_argument("invalid task move target");
            std::map<std::string, std::string> moved;
            for (auto i = files.begin(); i != files.end();) {
                if (i->first == path || i->first.compare(0, path.size()+1, path+"/") == 0) {
                    moved[target + i->first.substr(path.size())] = i->second; i = files.erase(i);
                } else ++i;
            }
            if (moved.empty()) throw std::invalid_argument("task move source missing");
            for (const auto &entry : moved) files[entry.first] = entry.second;
            parents(target);
        } else throw std::invalid_argument("unknown task proposal operation");
    }
    std::string revision() const
    {
        std::string manifest = "libai-task-artifacts-v1";
        for (const auto &item : files)
            manifest += std::to_string(item.first.size()) + ":" + item.first + ":" + item.second + ";";
        return agent_workspace_t::content_sha256(manifest);
    }
};
void evidence(acl::json_node &out, const char *name, const std::string &state,
    const std::string &revision, const char *authority, const std::string &reference)
{
    auto &n = out.add_child(name, true);
    n.add_text("state", state.c_str()).add_text("revision", revision.c_str())
        .add_text("authority", authority).add_text("reference", reference.c_str());
}
bool build_command(const std::string &id)
{
    const std::set<std::string> ids = {"go.build","rust.build","java.compile-main",
        "kotlin.compile-main","swift.build","csharp.build","d.build","objective-c.build",
        "javascript.syntax-check","python.syntax-check","php.syntax-check","c.make","cpp.make",
        "c.cmake.build","cpp.cmake.build","project.script-build"};
    return ids.count(id) != 0;
}
bool test_command(const std::string &id)
{
    const std::set<std::string> ids = {"go.test","rust.test","java.test","kotlin.test",
        "swift.test","csharp.test","d.test","javascript.test","python.test","php.test",
        "c.cmake.test","cpp.cmake.test","c.ctest","cpp.ctest","c.test","cpp.test"};
    return ids.count(id) != 0;
}
std::string report_state(acl::json_node &a, const char *key,
    const std::string &revision, bool test)
{
    auto *report = a[key];
    if (!report) return "unknown";
    report = object(report);
    if (text(*report, "artifact_revision") != revision) return "unknown";
    auto *commands = array_value((*report)["commands"]);
    if (!commands || !commands->is_array()) return "unknown";
    bool found = false;
    for (auto *entry = commands->first_child(); entry; entry = commands->next_child()) {
        auto *c = object(entry);
        const auto id = text(*c, "command_id");
        if (!(test ? test_command(id) : build_command(id))) continue;
        found = true;
        if (!boolean(*c, "started", false) || !boolean(*c, "ok", false) ||
            number(*c, "exit_code", -1) != 0 || boolean(*c, "cancelled", false) ||
            boolean(*c, "timed_out", false)) return "failed";
    }
    return found ? "passed" : "unknown";
}
}
bool tasks(const std::string &root, const std::string &op, acl::json_node &a,
    acl::json_node &out, std::string &error)
{
    (void)error;
    inventory tree(root); tree.scan("", 0);
    auto *changes = array_value(a["changes"]);
    std::map<std::string, acl::json_node *> latest;
    if (changes) {
        if (!changes->is_array()) throw std::invalid_argument("changes must be an array");
        for (auto *entry = changes->first_child(); entry; entry = changes->next_child()) {
            auto *c = object(entry);
            latest[text(*c, "relative_path")] = c;
        }
    }
    bool all_reviewed = !latest.empty(), rejected = false;
    for (auto &item : latest) {
        tree.overlay(*item.second);
        const auto review = text(*item.second, "review_status");
        rejected = rejected || review == "rejected";
        all_reviewed = all_reviewed && review == "accepted" &&
            !text(*item.second, "accepted_by").empty() &&
            !text(*item.second, "review_reference").empty();
    }
    const auto revision = tree.revision();
    out.add_text("artifact_revision", revision.c_str());
    if (op == "tasks.revision") return true;
    if (op != "tasks.describe_run") return false;
    const auto status = text(a, "status"), id = text(a, "id");
    const bool finished = !status.empty() && status != "running";
    const auto outcome = status == "success" ? "completed" :
        status == "failed" || status == "interrupted" ? "model_error" : status;
    out.add_bool("loop_finished", finished)
        .add_text("loop_outcome", outcome.c_str());
    // Requirements belong to host policy and remain conservative.
    auto &requirements = out.add_child("requirements", true);
    requirements.add_bool("generation", true).add_bool("build", true)
        .add_bool("tests", true).add_bool("review", true);
    evidence(out, "generation", outcome == "completed" && !latest.empty() ? "completed" : "unknown",
        revision, "host", id.empty() ? "" : id + ":proposals");
    evidence(out, "build", report_state(a, "validation", revision, false),
        revision, "host", id.empty() ? "" : id + ":validation");
    evidence(out, "tests", report_state(a, "test_validation", revision, true),
        revision, "host", id.empty() ? "" : id + ":tests");
    evidence(out, "review", all_reviewed ? "accepted" : rejected ? "rejected" : "unknown",
        revision, "human", all_reviewed && !id.empty() ? id + ":reviews" : "");
    // Decode the completed wire object: ACL builder nodes differ from parsed tag nodes.
    const std::string wire = out.to_string().c_str();
    acl::json contract(wire.c_str());
    if (!contract.finish()) throw std::runtime_error("cannot decode derived task contract");
    return task_contract(contract.get_root(), out);
}
}}}
