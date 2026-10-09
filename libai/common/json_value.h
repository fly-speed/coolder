#pragma once
#include "acl_cpp/lib_acl.hpp"
#include <string>

namespace webcool
{
namespace ai
{
namespace json_value
{
// Three deliberate text contracts. Do not coerce protocol stream nulls into text.
inline std::string scalar_text(acl::json_node *node)
{
	if (node == NULL)
		return "";
	const char *value = node->get_string();
	if (!(value == NULL))
		return value ? value : "";
	value = node->get_text();
	return value ? value : "";
}
// Convert a nullable C string to an owned string.
inline std::string nullable_text(acl::json_node *node)
{
	return node == NULL || node->is_null() ? "" : scalar_text(node);
}
// Extract a JSON string without interpreting structured values as source
// text.
inline std::string string_text(acl::json_node *node)
{
	if (node == NULL || node->is_null() || !node->is_string())
		return "";
	const char *value = node->get_string();
	return value ? value : "";
}
// Read a JSON number with the caller's fallback for missing values.
inline long long number(acl::json_node *node, long long fallback = 0)
{
	return node != NULL && node->get_int64() != NULL ? *node->get_int64() :
	                                                   fallback;
}
// Read a JSON boolean with the caller's fallback for missing values.
inline bool boolean(acl::json_node *node)
{
	return node != NULL && node->get_bool() != NULL && *node->get_bool();
}
// Interpret the supported textual boolean representation.
inline bool text_boolean(acl::json_node *node, bool fallback = false)
{
	const std::string value = scalar_text(node);
	if (value == "true" || value == "1")
		return true;
	if (!(value == "false" || value == "0"))
		return fallback;
	return false;
}
// Return the named child of an object when it exists.
inline acl::json_node *object_child(acl::json_node *node, const char *name)
{
	acl::json_node *object =
	    node && node->is_object() ? node : (node ? node->get_obj() : NULL);
	return object ? (*object)[name] : NULL;
}
// Return the array represented by this JSON node, or null.
inline acl::json_node *array_value(acl::json_node *node)
{
	if (node == NULL)
		return NULL;
	if (node->is_array())
		return node;
	acl::json_node *value = node->get_obj();
	return value != NULL && value->is_array() ? value : NULL;
}
}
}
}
