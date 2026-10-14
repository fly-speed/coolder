#pragma once
// Private compatibility adapter for the in-repository desktop host.
#ifndef LIBAI_CPP_HOST_INTERNAL
#error "This internal host integration is not part of the installed SDK"
#endif
#include <libai_cpp/agent.h>
#include "platform.h"
#include <libai/agent/agent_registry.h>
#include <libai/agent/ai_admin_policy.h>
#include <libai/common/ai_error_log.h>
#include <libai/common/file_ops.h>
#include <libai/common/identifiers.h>
#include <libai/common/json_values.h>
#include <libai/common/native_mutex.h>
#include <libai/project/agent_project_store.h>
#include <libai/project/project_diagnostics.h>
#include <libai/project/project_plan_template.h>
#include <libai/project/project_scaffold.h>
#include <libai/project/project_toolchain.h>
#include <libai/provider/ai_provider_client.h>
#include <libai/provider/ai_provider_store.h>
#include <libai/runtime/coding_runtime.h>
#include <libai/sandbox/program_sandbox.h>
#include <libai/sandbox/sandbox_execution.h>
#include <libai/storage/agent_run_store.h>
#include <libai/workspace/agent_change_limits.h>
#include <libai/workspace/agent_workspace.h>
#include <libai/workspace/workspace_change_set.h>
#include <libai/workspace/workspace_patch.h>
namespace libai
{
namespace cpp
{
// Keep host access behind the binding without copying core declarations.
namespace coding = ::libai::ai;
// Internal runtime compatibility; third-party applications use public APIs.
namespace runtime = ::action::agent_detail;
// Cooperative mutex owned by the facade's desktop implementation.
using Mutex = ::libai::mutex;
}
}
