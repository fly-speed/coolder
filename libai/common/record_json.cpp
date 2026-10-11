#include "record_json.h"
#include "record_codec.h"
#include "json_value.h"
#include <map>
#include <vector>
#include <stdexcept>
namespace webcool
{
namespace ai
{
namespace record_json
{
namespace
{
using specs = std::vector<std::string>;
// h=decoded UTF-8 text, n=nonnegative integer, b=boolean, s=literal text.
const std::map<std::string, specs> tables = {
	{ "WEBCOOL_AGENT_RUNS_V2",
	    { "h:id", "h:agent_id", "h:agent_version", "h:status",
	        "h:provider_id", "h:model", "h:project_path", "n:started_at",
	        "n:finished_at", "n:input_tokens", "n:output_tokens",
	        "n:latency_ms", "n:tool_calls", "n:proposed_changes",
	        "n:rejected_changes", "h:error", "n:cached_input_tokens",
	        "n:reasoning_tokens", "h:provider_error_category",
	        "n:provider_http_status", "b:provider_error_retryable" } },
	{ "WEBCOOL_AGENT_SESSIONS_V6",
	    { "h:id", "h:title", "h:agent_id", "h:provider_id",
	        "h:project_path", "h:summary", "h:messages", "h:last_run_id",
	        "n:created_at", "n:updated_at", "n:turn_count" } },
	{ "WEBCOOL_AGENT_PROJECTS_V1/P",
	    { "s:type", "h:id", "h:title", "h:project_path", "h:language",
	        "h:platform", "h:goal", "h:status", "n:created_at",
	        "n:updated_at", "n:plan_version" } },
	{ "WEBCOOL_AGENT_PROJECTS_V1/M",
	    { "s:type", "h:project_id", "h:id", "h:name", "h:layer", "h:path",
	        "h:dependencies" } },
	{ "WEBCOOL_AGENT_PROJECTS_V1/T",
	    { "s:type", "h:project_id", "h:id", "h:title", "h:module_id",
	        "h:status", "h:dependencies", "h:acceptance_criteria",
	        "h:test_plan" } },
	{ "WEBCOOL_AGENT_WORKFLOW_V1/H",
	    { "s:type", "s:project_id", "s:session_id", "s:task_id",
	        "s:failed_exit_code", "n:updated_at", "h:failed_command_id" } },
	{ "WEBCOOL_AGENT_WORKFLOW_V1/Q", { "s:type", "h:command_id" } },
	{ "WEBCOOL_AGENT_WORKFLOW_V1/D",
	    { "s:type", "h:path", "n:line", "n:column", "s:severity" } },
	{ "WEBCOOL_AI_PROVIDERS_V1",
	    { "h:id", "h:name", "h:protocol", "h:base_url", "h:model",
	        "h:api_key_ciphertext", "h:api_key_hint", "b:enabled",
	        "b:allow_file_content", "b:is_default", "b:responses_store",
	        "b:responses_background", "b:responses_compact",
	        "b:responses_strict_tools", "h:responses_min_reasoning_effort",
	        "h:responses_reasoning_summary", "h:responses_text_verbosity",
	        "h:responses_service_tier", "h:responses_cache_ttl",
	        "h:openai_organization", "h:openai_project",
	        "b:qwen_session_cache" } },
	{ "WEBCOOL_AI_PROVIDER_HEALTH_V1",
	    { "h:id", "h:last_test_status", "n:last_test_at",
	        "n:last_test_http_status", "n:last_test_latency_ms",
	        "h:last_test_error" } },
	{ "WEBCOOL_WORKSPACE_PATCH_V1",
	    { "n:created_at", "s:id", "h:path", "s:original_sha256",
	        "s:proposed_sha256", "h:content" } },
	{ "WEBCOOL_AGENT_CHECKPOINT_V1", { "s:ciphertext" } },
	{ "WEBCOOL_SANDBOX_PLAN_V1/H",
	    { "s:type", "n:created_at", "s:id", "h:project_path",
	        "h:command_id", "h:executable", "n:argument_count" } },
	{ "WEBCOOL_SANDBOX_PLAN_V1/A", { "s:type", "h:argument" } },
	{ "WEBCOOL_WORKSPACE_CHANGE_SET_V2/H",
	    { "s:type", "n:created_at", "s:id", "n:change_count" } },
	{ "WEBCOOL_WORKSPACE_CHANGE_SET_V2/C",
	    { "s:type", "s:operation", "h:path", "h:target_path",
	        "b:creates_file", "s:original_sha256", "s:proposed_sha256",
	        "h:reason", "h:content" } },
	{ "WEBCOOL_WORKSPACE_CHANGE_JOURNAL_V2/H",
	    { "s:type", "s:id", "n:change_count" } },
	{ "WEBCOOL_WORKSPACE_CHANGE_JOURNAL_V2/C",
	    { "s:type", "s:operation", "h:path", "h:target_path",
	        "b:creates_file", "s:original_sha256", "s:proposed_sha256",
	        "s:original_content_hex" } },
	{ "WEBCOOL_AGENT_PROJECT_INDEX_V1/H",
	    { "s:type", "h:project_id", "h:project_path", "n:revision",
	        "n:indexed_at", "n:directory_count",
	        "n:skipped_directory_count", "n:changed_file_count",
	        "b:truncated" } },
	{ "WEBCOOL_AGENT_PROJECT_INDEX_V1/E",
	    { "s:type", "h:path", "h:module_id", "h:kind", "h:language",
	        "n:size", "n:modified_at" } },
	{ "WEBCOOL_AGENT_PROJECT_INDEX_V1/S",
	    { "s:type", "h:path", "n:line", "h:kind", "h:text" } },
	{ "EXTERNAL_PROJECT_ROOTS", { "h:path", "h:external" } }
};
std::vector<std::string> lines(const std::string &value)
{
	std::istringstream in(value);
	std::vector<std::string> result;
	std::string line;
	while (std::getline(in, line))
		result.push_back(line);
	return result;
}
bool multiline(const std::string &format)
{
	return format == "WEBCOOL_WORKSPACE_PATCH_V1" ||
	    format == "WEBCOOL_AGENT_CHECKPOINT_V1" ||
	    format == "WEBCOOL_SANDBOX_PLAN_V1" ||
	    format == "WEBCOOL_WORKSPACE_CHANGE_SET_V2" ||
	    format == "WEBCOOL_WORKSPACE_CHANGE_JOURNAL_V2";
}
std::vector<std::vector<std::string>> split_records(
    const std::string &format, const std::vector<std::string> &source)
{
	std::vector<std::vector<std::string>> result;
	if (!multiline(format)) {
		for (const auto &line : source) {
			std::vector<std::string> row;
			record_codec::split_tabs(line, row);
			result.push_back(row);
		}
		return result;
	}
	if (format == "WEBCOOL_WORKSPACE_PATCH_V1" ||
	    format == "WEBCOOL_AGENT_CHECKPOINT_V1") {
		result.push_back(source);
		return result;
	}
	const size_t head = format == "WEBCOOL_SANDBOX_PLAN_V1" ?
	    6 :
	    (format == "WEBCOOL_WORKSPACE_CHANGE_JOURNAL_V2" ? 2 : 3);
	const size_t width = format == "WEBCOOL_SANDBOX_PLAN_V1" ?
	    1 :
	    (format == "WEBCOOL_WORKSPACE_CHANGE_JOURNAL_V2" ? 7 : 8);
	if (source.size() < head || (source.size() - head) % width != 0)
		throw std::runtime_error("invalid multiline record");
	std::vector<std::string> row{ "H" };
	row.insert(row.end(), source.begin(), source.begin() + head);
	result.push_back(row);
	for (size_t i = head; i < source.size(); i += width) {
		row = { format == "WEBCOOL_SANDBOX_PLAN_V1" ? "A" : "C" };
		row.insert(
		    row.end(), source.begin() + i, source.begin() + i + width);
		result.push_back(row);
	}
	return result;
}
const specs *schema(
    const std::string &format, const std::vector<std::string> &fields)
{
	auto it = tables.find(format + (fields.empty() ? "" : "/" + fields[0]));
	if (it == tables.end())
		it = tables.find(format);
	return it == tables.end() || it->second.size() != fields.size() ?
	    nullptr :
	    &it->second;
}
void cell(
    acl::json_node &row, const std::string &spec, const std::string &value)
{
	const auto name = spec.substr(2);
	std::string decoded;
	if (spec[0] == 'h') {
		if (!record_codec::hex_decode(value, decoded))
			throw std::runtime_error(
			    "invalid hexadecimal record field");
		row.add_text(name.c_str(), decoded.c_str());
	} else if (spec[0] == 'n') {
		long long n = 0;
		if (!record_codec::parse_nonnegative_number(value, n))
			throw std::runtime_error("invalid record number");
		row.add_number(name.c_str(), n);
	} else if (spec[0] == 'b') {
		if (value != "0" && value != "1")
			throw std::runtime_error("invalid record boolean");
		row.add_bool(name.c_str(), value == "1");
	} else
		row.add_text(name.c_str(), value.c_str());
}

std::string decode_cell(acl::json_node &row, const std::string &spec)
{
	auto *v = row[spec.substr(2).c_str()];
	if (!v)
		throw std::runtime_error("missing record JSON field");
	if (spec[0] == 'h' || spec[0] == 's') {
		if (!v->is_string())
			throw std::runtime_error("invalid record JSON text");
		const auto value = json_value::string_text(v);
		return spec[0] == 'h' ? record_codec::hex_encode(value) : value;
	}
	if (spec[0] == 'b') {
		if (!v->is_bool())
			throw std::runtime_error("invalid record JSON boolean");
		return *v->get_bool() ? "1" : "0";
	}
	if (!v->get_int64() || *v->get_int64() < 0)
		throw std::runtime_error("invalid record JSON number");
	return std::to_string(*v->get_int64());
}
std::vector<std::string> decode_fields(
    acl::json_node &row, const std::string &format)
{
	std::vector<std::string> fields;
	auto *values = json_value::array_value(row["fields"]);
	if (values) {
		for (auto *v = values->first_child(); v;
		     v = values->next_child()) {
			if (!v->is_string())
				throw std::runtime_error(
				    "invalid record JSON field");
			fields.push_back(json_value::string_text(v));
		}
		return fields;
	}
	const auto type = json_value::string_text(row["type"]);
	auto found = tables.find(format + (type.empty() ? "" : "/" + type));
	if (found == tables.end())
		throw std::runtime_error("unknown record JSON schema");
	for (const auto &spec : found->second)
		fields.push_back(decode_cell(row, spec));
	return fields;
}
std::string encode_master_key(const std::string &text)
{
	auto source = lines(text);
	acl::json json;
	auto &root = json.create_node();
	root.add_number("version", 1)
	    .add_text("format", "WEBCOOL_AI_MASTER_KEY_V2");
	if (source.size() == 1)
		root.add_text("active", source[0].c_str())
		    .add_text("secondary", "")
		    .add_text("phase", "");
	else if (source.size() == 4 &&
	    source[0] == "WEBCOOL_AI_MASTER_KEY_V2") {
		for (size_t i = 1; i < source.size(); ++i) {
			const auto at = source[i].find('=');
			if (at == std::string::npos)
				throw std::runtime_error(
				    "invalid master key journal");
			root.add_text(source[i].substr(0, at).c_str(),
			    source[i].substr(at + 1).c_str());
		}
	} else
		throw std::runtime_error("invalid master key journal");
	return root.to_string().c_str();
}

}
std::string encode(const std::string &text, const std::string &path)
{
	if (path.size() >= 11 &&
	    path.substr(path.size() - 11) == "preferences") {
		auto source = lines(text);
		if (source.size() > 1)
			throw std::runtime_error(
			    "invalid provider preferences");
		acl::json json;
		auto &root = json.create_node();
		root.add_text("ai_coding_provider_id",
		    source.empty() ? "" : source[0].c_str());
		return root.to_string().c_str();
	}
	if (path.find("ai-policy.v1") != std::string::npos) {
		acl::json json;
		auto &root = json.create_node();
		for (const auto &line : lines(text)) {
			const auto at = line.find('=');
			if (at == std::string::npos)
				throw std::runtime_error(
				    "invalid policy record");
			const auto key = line.substr(0, at),
			           value = line.substr(at + 1);
			long long n = 0;
			if (key == "enabled" ||
			    key.compare(0, 6, "allow_") == 0)
				root.add_bool(key.c_str(),
				    value == "1" || value == "true");
			else if (record_codec::parse_nonnegative_number(
			             value, n))
				root.add_number(key.c_str(), n);
			else
				root.add_text(key.c_str(), value.c_str());
		}
		return root.to_string().c_str();
	}
	if (path.find("ai_master.key") != std::string::npos)
		return encode_master_key(text);
	auto source = lines(text);
	std::string format;
	if (path.find("external-project-roots.v1") != std::string::npos)
		format = "EXTERNAL_PROJECT_ROOTS";
	else {
		if (source.empty() || source[0].compare(0, 8, "WEBCOOL_") != 0)
			throw std::runtime_error(
			    "unknown structured record format");
		format = source[0];
		source.erase(source.begin());
	}
	acl::json json;
	auto &root = json.create_node();
	root.add_number("version", 1).add_text("format", format.c_str());
	auto &records = json.create_array();
	root.add_child("records", records);
	for (const auto &fields : split_records(format, source)) {
		auto &row = records.add_child(false, true);
		const auto *description = schema(format, fields);
		if (description) {
			for (size_t i = 0; i < fields.size(); ++i)
				cell(row, (*description)[i], fields[i]);
		} else {
			auto &values = json.create_array();
			for (const auto &value : fields)
				values.add_child(
				    json.create_array_text(value.c_str()));
			row.add_child("fields", values);
		}
	}
	return root.to_string().c_str();
}
std::string decode(const std::string &text)
{
	if (text.find_first_not_of(" \t\r\n") == std::string::npos ||
	    text[text.find_first_not_of(" \t\r\n")] != '{')
		return text;
	acl::json json(text.c_str());
	if (!json.finish() || !json.get_root().is_object())
		throw std::runtime_error("invalid record JSON");
	if (json["ai_coding_provider_id"]) {
		if (!json["ai_coding_provider_id"]->is_string())
			throw std::runtime_error(
			    "invalid provider preferences JSON");
		const auto provider =
		    json_value::string_text(json["ai_coding_provider_id"]);
		return provider.empty() ? "" : provider + "\n";
	}
	if (!json["format"]) {
		std::ostringstream policy;
		auto &root = json.get_root();
		for (auto *v = root.first_child(); v; v = root.next_child()) {
			if (!v->tag_name() ||
			    (!v->is_string() && !v->is_number() &&
			        !v->is_bool()))
				throw std::runtime_error(
				    "invalid policy JSON value");
			const auto value = v->is_bool() ?
			    (*v->get_bool() ? "1" : "0") :
			    json_value::scalar_text(v);
			if (value.find_first_of("\r\n") != std::string::npos)
				throw std::runtime_error(
				    "invalid policy line break");
			policy << v->tag_name() << '=' << value << '\n';
		}
		return policy.str();
	}
	if (json_value::string_text(json["format"]) ==
	    "WEBCOOL_AI_MASTER_KEY_V2") {
		if (json_value::number(json["version"]) != 1)
			throw std::runtime_error(
			    "unsupported master key JSON version");
		const auto active = json_value::string_text(json["active"]),
		           secondary =
		               json_value::string_text(json["secondary"]);
		if (secondary.empty())
			return active + "\n";
		return "WEBCOOL_AI_MASTER_KEY_V2\nactive=" + active +
		    "\nsecondary=" + secondary +
		    "\nphase=" + json_value::string_text(json["phase"]) + "\n";
	}
	if (json_value::number(json["version"]) != 1)
		throw std::runtime_error("invalid record JSON version");
	const auto format = json_value::string_text(json["format"]);
	auto *records = json_value::array_value(json["records"]);
	if (format.empty() || !records)
		throw std::runtime_error("invalid record JSON schema");
	std::ostringstream out;
	if (format != "EXTERNAL_PROJECT_ROOTS")
		out << format << '\n';
	for (auto *row = records->first_child(); row;
	     row = records->next_child()) {
		const auto fields = decode_fields(*row, format);
		const bool multi = multiline(format);
		const size_t begin = multi && (*row)["type"] ? 1 : 0;
		for (size_t i = begin; i < fields.size(); ++i) {
			if (i > begin)
				out << (multi ? '\n' : '\t');
			out << fields[i];
		}
		out << '\n';
	}
	return out.str();
}
input::input(const std::string &path, std::ios::openmode mode)
{
	std::ifstream file(path.c_str(), mode);
	if (!file.good()) {
		setstate(std::ios::failbit);
		return;
	}
	try {
		std::string text((std::istreambuf_iterator<char>(file)), {});
		str(decode(text));
	} catch (const std::exception &) {
		str("INVALID_JSON_RECORD\n");
	}
}
output::output(const std::string &path, std::ios::openmode mode)
        : file_(path.c_str(), mode)
        , path_(path)
{
	if (!file_.good())
		setstate(std::ios::failbit);
}
void output::close()
{
	if (closed_)
		return;
	closed_ = true;
	try {
		if (good())
			file_ << encode(str(), path_) << '\n';
		else
			file_.setstate(std::ios::failbit);
		file_.close();
		if (!file_.good())
			setstate(std::ios::failbit);
	} catch (const std::exception &) {
		file_.close();
		setstate(std::ios::failbit);
	}
}
output::~output()
{
	close();
}
}
}
}
