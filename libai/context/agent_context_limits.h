#pragma once
#include <cstddef>
namespace webcool
{
namespace ai
{
// Shared by compaction, checkpoint persistence and provider admission.
const size_t kMaxAgentTranscriptBytes = 256 * 1024;
// Leave room for capability snapshots and protocol recovery instructions.
const size_t kMaxAgentPromptBytes = kMaxAgentTranscriptBytes + 32 * 1024;
}
}
