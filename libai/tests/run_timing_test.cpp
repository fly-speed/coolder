#include "../storage/run_timing.h"
#include <cassert>
int main() {
    using libai::ai::run_timing;
    assert(run_timing(100,110,"completed",500).elapsed_ms==10000);
    assert(run_timing(100,110,"completed",900).elapsed_ms==10000);
    assert(run_timing(100,0,"running",107).elapsed_ms==7000);
    assert(!run_timing(100,0,"failed",500).available);
    assert(!run_timing(110,100,"cancelled",500).available);
    assert(!run_timing(0,100,"completed",500).available);
    assert(run_timing(100,100,"completed",500).available);
    assert(!run_timing(1,std::numeric_limits<long long>::max(),"completed").available);
}
