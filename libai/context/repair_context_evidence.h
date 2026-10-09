#pragma once
#include "../provider/ai_provider_client.h"
#include "acl_cpp/lib_acl.hpp"

namespace webcool
{
namespace ai
{
// Compare exact source bytes, including their JSON-escaped representations in
// batch proposals and compact source records. Never infer freshness from path alone.
inline bool contains_source_evidence(const std::string &evidence,
				     const std::string &path,
				     const std::string &content)
{
	if (content.empty() || evidence.find(path) == std::string::npos)
		return false;
	for (const char *label :
	     { "compiler diagnostic source", "current staged source" }) {
		const std::string record = "--- " + path + " (" + label +
					   ") ---\n" + content + "\n";
		const size_t at = evidence.find(record);
		if (at != std::string::npos) {
			const std::string tail =
				evidence.substr(at + record.size());
			if (tail.empty() || tail.find("--- ") == 0 ||
			    tail.find("</repair_source_context>") == 0 ||
			    tail.find("[remaining repair source ") == 0)
				return true;
		}
	}
	acl::json parsed(evidence.c_str());
	const std::string normalized =
		parsed.finish() ? parsed.to_string().c_str() : evidence;
	acl::json source_json;
	acl::json_node &source = source_json.create_node();
	source.add_text("content", content.c_str());
	const std::string object = source.to_string().c_str();
	std::string encoded = object.substr(1, object.size() - 2);
	for (int depth = 0; depth < 4; ++depth) {
		// ACL may parse just one JSON fragment in a mixed transcript. Never
		// discard the original text containing the other source records.
		if (evidence.find(encoded) != std::string::npos ||
		    normalized.find(encoded) != std::string::npos)
			return true;
		acl::json json;
		acl::json_node &root = json.create_node();
		root.add_text("v", encoded.c_str());
		const std::string wrapped = root.to_string().c_str();
		encoded = wrapped.substr(6, wrapped.size() - 8);
	}
	return false;
}

inline bool request_has_source(const completion_request_t &request,
			       const std::string &path,
			       const std::string &content)
{
	if (contains_source_evidence(request.user_prompt, path, content))
		return true;
	// The latest developer guidance has not entered tool_history yet.
	if (contains_source_evidence(request.turn_instructions, path, content))
		return true;
	for (const auto &exchange : request.tool_history) {
		if (contains_source_evidence(exchange.preceding_instructions,
					     path, content))
			return true;
		for (const auto &call : exchange.calls)
			if ((call.path == path && call.content == content) ||
			    contains_source_evidence(call.content, path,
						     content))
				return true;
		for (const auto &output : exchange.outputs)
			if (contains_source_evidence(output.output, path,
						     content))
				return true;
	}
	return false;
}
}
}
