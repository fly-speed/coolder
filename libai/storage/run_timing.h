#pragma once
#include <ctime>
#include <limits>
#include <string>
namespace webcool { namespace ai {
// Legacy audit timestamps have second resolution; never imply monotonic precision.
struct run_timing_t {
    long long elapsed_ms = 0;
    bool available = false;
};
inline run_timing_t run_timing(long long started_at,long long finished_at,
    const std::string &status,long long now=static_cast<long long>(std::time(nullptr))) {
    run_timing_t result;
    const long long end=finished_at>0?finished_at:(status=="running"?now:0);
    if (started_at<=0 || end<started_at) return result;
    const auto seconds=end-started_at;
    if (seconds>std::numeric_limits<long long>::max()/1000) return result;
    result.elapsed_ms=seconds*1000;
    result.available=true;
    return result;
}
}}
