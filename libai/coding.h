#pragma once
#include "agent/agent_registry.h"
#include "workspace/agent_workspace.h"
#include "provider/ai_provider_client.h"
#include "agent/ai_admin_policy.h"
#include "runtime/coding_runtime.h"
namespace libai
{
namespace ai
{
// Library-owned entry point for the existing coding runtime.
namespace coding = ::action::agent_detail;
}
}
