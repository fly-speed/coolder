#pragma once
#include <algorithm>
#include <string>
namespace webcool { namespace ai {
struct provider_stream_progress_t {
    long long elapsed_ms = 0;
    long long headers_ms = -1;
    long long first_byte_ms = -1;
    long long last_data_ms = 0;
    long long last_effective_ms = 0;
    size_t received_bytes = 0;
    size_t text_bytes = 0;
    size_t reasoning_bytes = 0;
    size_t tool_argument_bytes = 0;
    std::string phase = "waiting_headers";
};
inline const char* provider_stream_deadline(const provider_stream_progress_t& p,
    long long idle_ms, long long stall_ms, long long total_ms) {
    if (p.elapsed_ms >= total_ms) return "request_total_timeout";
    if (p.elapsed_ms - p.last_data_ms >= idle_ms) return "connection_idle_timeout";
    if (p.elapsed_ms - p.last_effective_ms >= stall_ms) return "effective_output_stalled";
    return "";
}
} }
