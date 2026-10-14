#pragma once
#include "../stdafx.h"
#include "../common/json_values.h"
#include <stdexcept>
#include <cstdlib>
#include <cerrno>
#include <string>
#include <vector>
#include <functional>
#include <memory>
namespace libai {class execution_store_t;}
namespace libai
{
namespace ai
{
struct sandbox_command_t;
struct sandbox_limits_t;
struct sandbox_result_t;
namespace coding_detail
{
// Internal trusted command runner. The callback must use the reviewed command
// and existing sandbox broker; never expose this as an arbitrary model tool.
bool record_command_execution(const std::string &root,const std::string &project,
    const sandbox_command_t &command,const sandbox_limits_t &limits,
    const std::string &key,sandbox_result_t &result,
    const std::function<bool(sandbox_result_t &)> &dispatch,std::string &error,
    const std::string &parent_key = "");
bool inspect_command_parent(const std::string &root, acl::json_node &args,
    acl::json_node &out, std::string &error);
bool lock_command_children(const std::string &root, const std::string &project,
    const std::string &parent_key, const std::string &outcome,
    std::vector<std::shared_ptr<libai::execution_store_t>> &leases, std::string &error);
// Strict string argument decoder; never coerces objects into filesystem paths.
inline std::string text(acl::json_node &a, const char *key)
{
	auto *n = a[key];
	if (!n)
		return "";
	if (!n->is_string())
		throw std::invalid_argument(
		    std::string(key) + " must be a string");
	return json_values::text(n);
}
// Decode an optional boolean, preserving the caller-selected default.
inline bool boolean(acl::json_node &a, const char *key, bool fallback)
{
	auto *n = a[key];
	if (!n)
		return fallback;
	const std::string value = json_values::text(n);
	if (!n->is_bool() || (value != "true" && value != "false"))
		throw std::invalid_argument(
		    std::string(key) + " must be boolean");
	return value == "true";
}
// Decode an exact signed integer without silently truncating JSON floats.
inline long long number(
    acl::json_node &a, const char *key, long long fallback = 0)
{
	auto *n = a[key];
	if (!n)
		return fallback;
	if (!n->is_number())
		throw std::invalid_argument(
		    std::string(key) + " must be an integer");
	const auto value = json_values::text(n);
	char *end = nullptr;
	errno = 0;
	long long result = std::strtoll(value.c_str(), &end, 10);
	if (errno || end == value.c_str() || *end)
		throw std::invalid_argument("invalid integer argument");
	return result;
}
// Bound history listing to avoid unbounded foreign-language responses.
inline size_t limit(acl::json_node &a)
{
	auto value = number(a, "limit", 20);
	if (value < 0 || value > 1000)
		throw std::invalid_argument("limit must be between 0 and 1000");
	return static_cast<size_t>(value);
}
// Create a tagged array while returning the actual array rather than its tag wrapper.
inline acl::json_node &array(acl::json_node &out, const char *key)
{
	auto &items = out.get_json().create_array();
	out.add_child(key, items);
	return items;
}
// Unwrap ACL's tagged array representation without coercing other value types.
inline acl::json_node *array_value(acl::json_node *node)
{
	if (!node || node->is_array())
		return node;
	auto *value = json_value::array_value(node);
	if (!value)
		throw std::invalid_argument("argument must be an array");
	return value;
}
// Append a string vector as a JSON array.
inline void strings(
    acl::json_node &out, const char *key, const std::vector<std::string> &items)
{
	auto &array = out.get_json().create_array();
	for (const auto &item : items)
		array.add_child(out.get_json().create_array_text(item.c_str()));
	out.add_child(key, array);
}
// Derive host task evidence from a bounded project inventory and real command results.
bool tasks(const std::string &root, const std::string &op, acl::json_node &a,
    acl::json_node &out, std::string &error);
// Execute workspace, patch and reviewed transaction operations.
bool workspace(const std::string &root, const std::string &op,
    acl::json_node &a, acl::json_node &out, std::string &error);
// Execute project, scaffold, diagnostics and toolchain operations.
bool projects(const std::string &root, const std::string &op, acl::json_node &a,
    acl::json_node &out, std::string &error);
// Execute persistent session and metadata history operations.
bool storage(const std::string &root, const std::string &op, acl::json_node &a,
    acl::json_node &out, std::string &error);
// Trusted host receipt operation; never register as a model tool.
bool confirm_progress(const std::string &root, const std::string &arguments,
    acl::json_node &out, std::string &error);
// Execute fixed-command sandbox capabilities, execution and cancellation.
bool sandbox(const std::string &root, const std::string &op,
    acl::json_node &a, acl::json_node &out, std::string &error);
// Upgrade versioned private metadata to JSON, keeping protected legacy backups.
bool migrate(const std::string &root, acl::json_node &a, acl::json_node &out, std::string &error);
// Validate or configure trusted host runtime budgets and toolchain allowlists.
bool runtime(const std::string &op, acl::json_node &a, acl::json_node &out, std::string &error);
// Execute provider settings and native model operations.
bool providers(const std::string &root, const std::string &op,
    acl::json_node &a, acl::json_node &out, std::string &error);
}
}
}
