#pragma once
#include <cstddef>

#define WEBCOOL_PROPOSAL_BATCH_ITEMS 64
#define WEBCOOL_STRINGIFY_IMPL(x) #x
#define WEBCOOL_STRINGIFY(x) WEBCOOL_STRINGIFY_IMPL(x)

namespace webcool
{
namespace ai
{
// A run accumulates proposals across many model calls. This is deliberately
// separate from the bounded size of a single proposal batch.
constexpr std::size_t kMaxAgentChanges = 256;
constexpr std::size_t kMaxProposalBatchItems = WEBCOOL_PROPOSAL_BATCH_ITEMS;
constexpr std::size_t kMaxProposalFileBytes = 256 * 1024;
constexpr std::size_t kMaxProposalBatchBytes = 2 * 1024 * 1024;
constexpr std::size_t kMaxSavedProposalBytes = 4 * 1024 * 1024;
}
}
