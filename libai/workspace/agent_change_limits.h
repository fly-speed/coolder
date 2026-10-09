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
// Upper bound for proposal batch items.
constexpr std::size_t kMaxProposalBatchItems = WEBCOOL_PROPOSAL_BATCH_ITEMS;
// Upper bound for proposal file bytes.
constexpr std::size_t kMaxProposalFileBytes = 256 * 1024;
// Upper bound for proposal batch bytes.
constexpr std::size_t kMaxProposalBatchBytes = 2 * 1024 * 1024;
// Upper bound for saved proposal bytes.
constexpr std::size_t kMaxSavedProposalBytes = 4 * 1024 * 1024;
}
}
