#pragma once

namespace action
{

// Read-only workspace endpoints used by the UI. The model tool loop invokes the
// same underlying agent_workspace_t policy through ai_agent_actions.cpp.
class AiWorkspaceListAction {
public:
	static bool run(request_t &req, response_t &res);
};

class AiWorkspaceReadAction {
public:
	static bool run(request_t &req, response_t &res);
};

class AiWorkspaceSearchAction {
public:
	static bool run(request_t &req, response_t &res);
};

class AiProjectDirectoriesAction {
public:
	static bool run(request_t &req, response_t &res);
};

class AiWorkspaceProjectCreateAction {
public:
	// Creates a confirmed project in the workspace or an authorized local directory.
	static bool run(request_t &req, response_t &res);
};

class AiWorkspaceDirectoryCreateAction {
public:
	// Creates one child directory beneath the selected directory in the current
	// authenticated user's virtual disk. Only a single path component is allowed.
	static bool run(request_t &req, response_t &res);
};

class AiSandboxCapabilitiesAction {
public:
	static bool run(request_t &req, response_t &res);
};

class AiSandboxPlanAction {
public:
	// Creates a short-lived preview only; no process is started here.
	static bool run(request_t &req, response_t &res);
};

class AiSandboxExecuteAction {
public:
	// Requires `confirm: true` and atomically consumes the plan before execution.
	static bool run(request_t &req, response_t &res);
};

class AiSandboxRunStatusAction {
public:
	// Returns transient stdout/stderr only while the in-memory result is retained.
	static bool run(request_t &req, response_t &res);
};

class AiSandboxRunHistoryAction {
public:
	// Returns persistent metadata only; command output is never read from audit.
	static bool run(request_t &req, response_t &res);
};

class AiSandboxRunCancelAction {
public:
	// Requests process-group termination; the status endpoint reports completion.
	static bool run(request_t &req, response_t &res);
};

class AiWorkspacePatchPreviewAction {
public:
	static bool run(request_t &req, response_t &res);
};

class AiWorkspacePatchApplyAction {
public:
	static bool run(request_t &req, response_t &res);
};

class AiWorkspaceChangeSetPreviewAction {
public:
	// Accepts the user-selected items and returns a per-file combined preview.
	static bool run(request_t &req, response_t &res);
};

class AiWorkspaceChangeSetApplyAction {
public:
	// Requires confirm=true and consumes the transaction plan exactly once.
	static bool run(request_t &req, response_t &res);
};

} // namespace action
