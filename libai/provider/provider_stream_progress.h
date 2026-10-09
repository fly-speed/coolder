#pragma once
#include <algorithm>
#include <string>
namespace webcool
{
namespace ai
{
// Stream timing and output counters used to detect stalled generation.
struct provider_stream_progress_t {
	// Elapsed stream or operation time in milliseconds.
	long long elapsed_ms = 0;
	// Time to receiving the response headers in milliseconds.
	long long headers_ms = -1;
	// Time to the first response byte in milliseconds.
	long long first_byte_ms = -1;
	// Elapsed time when any stream data last arrived, in milliseconds.
	long long last_data_ms = 0;
	// Elapsed time when useful output last arrived, in milliseconds.
	long long last_effective_ms = 0;
	// Total response bytes received so far.
	size_t received_bytes = 0;
	// Bytes of user-facing text received so far.
	size_t text_bytes = 0;
	// Bytes of reasoning received so far.
	size_t reasoning_bytes = 0;
	// Bytes of streamed tool-call arguments received so far.
	size_t tool_argument_bytes = 0;
	// Current processing phase reported to progress observers.
	std::string phase = "waiting_headers";
};
// Identify an expired stream timeout from timing and progress counters.
inline const char *provider_stream_deadline(const provider_stream_progress_t &p,
    long long idle_ms, long long stall_ms, long long total_ms)
{
	if (p.elapsed_ms >= total_ms)
		return "request_total_timeout";
	if (p.elapsed_ms - p.last_data_ms >= idle_ms)
		return "connection_idle_timeout";
	if (!(p.elapsed_ms - p.last_effective_ms >= stall_ms))
		return "";
	return "effective_output_stalled";
}
}
}
