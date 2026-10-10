#include "provider/ai_provider_client_internal.h"
#include "validation/task_acceptance.h"
#include "context/task_delivery_summary.h"
#include "storage/task_contract_store.h"
#include "common/record_codec.h"
#include "common/file_ops.h"
#include "common/identifiers.h"
#include "common/json_value.h"
#include "workspace/diff_preview.h"
#include <cassert>
#include <fstream>
#ifndef _WIN32
#include <unistd.h>
#endif
using namespace webcool::ai;
int main()
{
	// Cache counts must normalize to an inclusive input total, and zero hits
	// must remain distinguishable from providers with no cache breakdown.
	{
		using namespace provider_detail;
		completion_result_t result;
		result.input_tokens = 100;
		acl::json chat(R"({"prompt_cache_hit_tokens":70,"prompt_cache_miss_tokens":30})");
		assert(chat.finish());
		parse_chat_cache_usage(&chat.get_root(), result);
		assert(result.input_tokens == 100 && result.cached_input_tokens == 70 && result.cache_usage_available);
		acl::json zero(R"({"prompt_tokens_details":{"cached_tokens":0}})");
		assert(zero.finish());
		parse_chat_cache_usage(&zero.get_root(), result);
		assert(result.cached_input_tokens == 0 && result.cache_usage_available);
		acl::json absent(R"({"prompt_tokens":100})");
		assert(absent.finish());
		completion_result_t unknown;
		parse_chat_cache_usage(&absent.get_root(), unknown);
		assert(!unknown.cache_usage_available);
		acl::json anthropic(R"({"input_tokens":10,"cache_read_input_tokens":70,"cache_creation_input_tokens":20,"output_tokens":5})");
		assert(anthropic.finish());
		parse_anthropic_usage(&anthropic.get_root(), result);
		assert(result.input_tokens == 100 && result.cached_input_tokens == 70 && result.output_tokens == 5);
		acl::json delta(R"({"output_tokens":8})");
		assert(delta.finish());
		parse_anthropic_usage(&delta.get_root(), result);
		assert(result.input_tokens == 100 && result.cached_input_tokens == 70 && result.output_tokens == 8);
		completion_result_t copied;
		copied.cached_input_tokens = 70;
		result.cached_input_tokens = 0;
		copy_usage(result, copied);
		assert(copied.cache_usage_available && copied.cached_input_tokens == 0);
		provider_config_t provider;
		provider.protocol = "openai_chat";
		std::vector<streamed_tool_call_t> calls;
		stream_diagnostics_t diagnostics;
		std::string text, reasoning, error;
		completion_result_t streamed;
		assert(parse_stream_line(provider, R"(data: {"choices":[],"usage":{"prompt_tokens":100,"completion_tokens":5,"prompt_cache_hit_tokens":70,"prompt_cache_miss_tokens":30}})",
		    streamed, calls, text, reasoning, diagnostics, error));
		assert(streamed.input_tokens == 100 && streamed.cached_input_tokens == 70 && streamed.cache_usage_available);
		provider.protocol = "anthropic_messages";
		streamed = completion_result_t();
		assert(parse_stream_line(provider, R"(data: {"type":"message_start","message":{"usage":{"input_tokens":10,"cache_read_input_tokens":70,"cache_creation_input_tokens":20}}})",
		    streamed, calls, text, reasoning, diagnostics, error));
		assert(parse_stream_line(provider, R"(data: {"type":"message_delta","usage":{"output_tokens":8}})",
		    streamed, calls, text, reasoning, diagnostics, error));
		assert(streamed.input_tokens == 100 && streamed.cached_input_tokens == 70 && streamed.output_tokens == 8);

	}

	acl::json progress_input(R"({"items":[
        {"requirement":"显示蛇","status":"implemented","evidence":"已连接玩家坐标到绘制函数","remaining":"浏览器确认"},
        {"requirement":"显示食物","status":"partial","evidence":"已接收食物数据","remaining":"绘制尚未接入"},
        {"requirement":"方格区域","status":"not_implemented","remaining":"尚未绘制网格"},
        {"requirement":"没有依据","status":"implemented"},
        {"requirement":"不能自行验收","status":"verified","independently_verified":true}
    ]})");
	assert(progress_input.finish());
	const std::string normalized =
	    normalize_requirement_progress(progress_input["items"]);
	const std::string progress_summary =
	    requirement_progress_summary(normalized, true);
	assert(progress_summary.find("显示蛇：已实现，待验收") !=
	    std::string::npos);
	assert(
	    progress_summary.find("显示食物：部分完成") != std::string::npos);
	assert(progress_summary.find("方格区域：未完成") != std::string::npos);
	assert(
	    progress_summary.find("没有依据：无法确认") != std::string::npos);
	assert(progress_summary.find("不能自行验收：无法确认") !=
	    std::string::npos);
	assert(normalized.find("\"independently_verified\":true") ==
	    std::string::npos);
	assert(requirement_progress_summary("", true).find("无法确认") !=
	    std::string::npos);
	assert(task_delivery_summary(std::string(10000, 'x'),
	           { { "write file", "" } }, std::string(10000, 'y'),
	           std::string(10000, 'z'), true, normalized)
	           .size() <= 4096);

	const std::string requirements =
	    "食物数量在线拉杆配置；速度慢、中、快；空格暂停/继续；失败后空格重新开始；多人玩。";
	const std::string delivery = task_delivery_summary(requirements,
	    { { "write index.html", "" } }, "食物滑杆已调整；多人同步尚未完成",
	    "检查通过，功能待验收", true);
	assert(delivery.find(requirements) != std::string::npos);
	assert(delivery.find("write index.html") != std::string::npos);
	assert(delivery.find("多人同步尚未完成") != std::string::npos);
	assert(delivery.find("验证结论") != std::string::npos);
	const std::string large_delivery =
	    task_delivery_summary(std::string(20000, 'x'),
	        std::vector<std::pair<std::string, std::string>>(
	            10, { std::string(1000, 'p'), std::string(1000, 'r') }),
	        std::string(5000, 's'), std::string(5000, 'v'), true);
	assert(large_delivery.size() <= 4096 &&
	    large_delivery.find("验证结论") != std::string::npos);

	assert(task_acceptance_status(false, true, true, false, false) ==
	    "pending_verification");
	assert(task_acceptance_status(true, false, true, false, false) ==
	    "pending_verification");
	assert(task_acceptance_status(true, true, true, false, false) ==
	    "checks_passed");
	assert(task_acceptance_status(true, true, false, false, false) ==
	    "verification_failed");
	assert(task_acceptance_status(true, true, false, true, true) ==
	    "scope_verified");

	std::string binary("a\0\xff", 3), decoded("old");
	assert(record_codec::hex_encode(binary) == "6100ff");
	assert(
	    record_codec::hex_decode("6100FF", decoded) && decoded == binary);
	assert(!record_codec::hex_decode("1", decoded) && decoded.empty());
	assert(!record_codec::hex_decode("61zz", decoded) && decoded == "a");
	std::vector<std::string> fields;
	record_codec::split_tabs("\ta\t", fields);
	assert(fields.size() == 3 && fields[0].empty() && fields[1] == "a" &&
	    fields[2].empty());
	long long value = 17;
	assert(!record_codec::parse_nonnegative_number("-1", value) &&
	    value == 17);
	assert(!record_codec::parse_nonnegative_number(
	           "9223372036854775808", value) &&
	    value == 17);
	assert(record_codec::parse_nonnegative_number(" +42", value) &&
	    value == 42);
	assert(identifiers::valid_id("ABCDEF0123456789abcdef0123456789"));
	assert(!identifiers::valid_id("") &&
	    !identifiers::valid_id(std::string(32, 'z')));
	const std::string first = identifiers::new_id(),
	                  second = identifiers::new_id();
	assert(identifiers::valid_id(first) && identifiers::valid_id(second) &&
	    first != second);
	acl::json json;
	json.update(
	    "{\"n\":null,\"s\":\"1\",\"i\":42,\"b\":true,\"o\":{\"x\":7},\"a\":[1]}");
	assert(json_value::string_text(json["i"]).empty());
	assert(json_value::scalar_text(json["i"]) == "42");
	assert(json_value::nullable_text(json["n"]).empty());
	assert(json_value::string_text(json["s"]) == "1");
	assert(!json_value::boolean(json["s"]) &&
	    json_value::text_boolean(json["s"]));
	assert(json_value::boolean(json["b"]));
	assert(json_value::text_boolean(NULL, true));
	assert(json_value::number(NULL, 19) == 19);
	assert(
	    json_value::number(json_value::object_child(json["o"], "x")) == 7);
	assert(json_value::array_value(json["a"]) != NULL);
	assert(json_value::array_value(json["s"]) == NULL);
	line_diff_preview_t diff("a\r\nb\n", "a\nc\n");
	assert(diff.removed_lines == 1 && diff.added_lines == 1);
	std::ostringstream preview;
	for (size_t i = 0; i < diff.operations.size(); ++i)
		diff.append_line(preview, diff.operations[i]);
	assert(preview.str() == "  1 1 | a\n- 2      | b\n+      2 | c\n");
	line_diff_preview_t unchanged("\n", "\n");
	assert(unchanged.old_lines.size() == 1 && !unchanged.visible[0]);
	assert(file_ops::join_path("", "a") == "a");
#ifndef _WIN32
	char temp[] = "/tmp/libai-primitives-XXXXXX";
	const char *root = mkdtemp(temp);
	assert(root);
	const std::string dir = std::string(root) + "/private",
	                  link = std::string(root) + "/link";
	assert(file_ops::make_private_directory(dir));
	struct stat st;
	assert(stat(dir.c_str(), &st) == 0 && (st.st_mode & 0777) == 0700);
	assert(symlink(dir.c_str(), link.c_str()) == 0);
	assert(!file_ops::safe_directory(link));
	const std::string from = dir + "/from", to = dir + "/to";
	{
		std::ofstream f(from.c_str());
		f << "new";
	}
	{
		std::ofstream f(to.c_str());
		f << "old";
	}
	assert(file_ops::replace_file(from, to));
	assert(!file_ops::replace_file(from, to));
	{
		std::ifstream f(to.c_str());
		std::string text;
		f >> text;
		assert(text == "new");
	}
	std::string contract, contract_error;
	assert(begin_task_contract(root, "private", "", first,
	    "Original user requirement", false, contract, contract_error));
	const std::string original_contract = contract;
	assert(begin_task_contract(root, "private", "", first,
	    "Original user requirement", true, contract, contract_error));
	assert(contract ==
	    original_contract); // Recovery cannot append the same request twice.
	assert(!begin_task_contract(root, "private", "", "../escape", "x",
	    false, contract, contract_error));
	assert(!begin_task_contract(root, "private", "", second,
	    std::string(65537, 'x'), false, contract, contract_error));
	const std::string contract_file =
	    dir + "/.webcool_agent/task-contract-" + first + ".json";
	{
		std::ofstream f(contract_file.c_str());
		f << "corrupt";
	}
	assert(!begin_task_contract(root, "private", "", first,
	    "Original user requirement", true, contract, contract_error));
	{
		std::ifstream f(contract_file.c_str());
		std::string value;
		f >> value;
		assert(value == "corrupt");
	}
	assert(unlink(contract_file.c_str()) == 0);
	assert(rmdir((dir + "/.webcool_agent").c_str()) == 0);
	assert(unlink(to.c_str()) == 0 && rmdir(dir.c_str()) == 0);
	assert(file_ops::path_entry_exists(link));
	assert(unlink(link.c_str()) == 0 && rmdir(root) == 0);
#endif
}
