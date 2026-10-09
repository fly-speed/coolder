#pragma once

#include <string>

namespace webcool
{
namespace ai
{

inline long long provider_max_output_tokens_from_error(const std::string &error)
{
	const char *markers[] = { "valid range of max_tokens is [1, ",
				  "valid range of max_output_tokens is [1, " };
	for (const char *marker : markers) {
		const size_t found = error.find(marker);
		if (found == std::string::npos)
			continue;
		size_t pos = found + std::string(marker).size();
		long long value = 0;
		const size_t digits = pos;
		while (pos < error.size() && error[pos] >= '0' &&
		       error[pos] <= '9') {
			if (value > 1000000)
				return 0;
			value = value * 10 +
				static_cast<long long>(error[pos++] - '0');
		}
		if (pos > digits && value >= 1 && value <= 1000000)
			return value;
	}
	return 0;
}

inline void annotate_output_token_limit(std::string &error,
					const std::string &incomplete_reason,
					long long effective_max_output_tokens)
{
	if (effective_max_output_tokens <= 0 ||
	    error.find("本轮实际输出 Token 上限") != std::string::npos) {
		return;
	}
	const bool exhausted =
		incomplete_reason == "max_output_tokens" ||
		error.find("response incomplete: max_output_tokens") !=
			std::string::npos ||
		error.find("entire output-token budget") != std::string::npos ||
		error.find("exhausted the output-token budget") !=
			std::string::npos;
	if (!exhausted)
		return;
	error +=
		" 本轮实际输出 Token 上限为 " +
		std::to_string(effective_max_output_tokens) +
		"（即请求发送给服务商的 max_output_tokens）；模型在生成完整答复或工具调用前已达到该上限。"
		"可在“系统设置 → AI智能体策略 → 单次最大输出 Token”中提高允许上限，"
		"并重新开始或从断点继续。";
}

}
}
