#pragma once
#include "json_value.h"
namespace webcool
{
namespace ai
{
namespace json_values
{
// Common scalar conversion used by host adapters and the coding bridge.
inline std::string text(acl::json_node *node)
{
	return json_value::scalar_text(node);
}
// Common textual boolean conversion with an explicit host fallback.
inline bool boolean(acl::json_node *node, bool fallback)
{
	return json_value::text_boolean(node, fallback);
}
}
}
}
