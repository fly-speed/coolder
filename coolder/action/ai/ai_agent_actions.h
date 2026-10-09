#pragma once

namespace action
{

// Starts an asynchronous model/tool loop and returns a run ID immediately.
class AiAgentRunStartAction {
public:
	static bool run(request_t &req, response_t &res);
};

class AiAgentRunListAction {
public:
	static bool run(request_t &req, response_t &res);
};

class AiAgentRunStatusAction {
public:
	static bool run(request_t &req, response_t &res);
};

class AiAgentRunEventsAction {
public:
	// Authenticated, per-user SSE stream containing metadata-only progress.
	static bool run(request_t &req, response_t &res);
};

class AiAgentRunCancelAction {
public:
	// Cancellation is cooperative at provider/tool boundaries.
	static bool run(request_t &req, response_t &res);
};

class AiAgentRunPauseAction {
public:
	// Pauses at the next safe provider/tool boundary and preserves all state.
	static bool run(request_t &req, response_t &res);
};

class AiAgentRunResultDecisionAction {
public:
	// Records accept/reject without deleting the project-local generated result.
	static bool run(request_t &req, response_t &res);
};

class AiAgentRunResultReviewAction {
public:
	// Persists accepted/rejected state for exact file generations.
	static bool run(request_t &req, response_t &res);
};

class AiAgentRunResultApplyAction {
public:
	// Recovers a persisted result whose atomic source write was interrupted.
	static bool run(request_t &req, response_t &res);
};

class AiAgentRunReasoningSaveAction {
public:
	// Saves the current reasoning snapshot to a fixed .txt file in its project.
	static bool run(request_t &req, response_t &res);
};

class AiAssistantSessionListAction {
public:
	static bool run(request_t &req, response_t &res);
};

class AiAssistantImageAction {
public:
	static bool run(request_t &req, response_t &res);
};

class AiAssistantSessionTagAction {
public:
	static bool run(request_t &req, response_t &res);
};

class AiAssistantSessionUpdateAction {
public:
	static bool run(request_t &req, response_t &res);
};

class AiAssistantSessionImportAction {
public:
	static bool run(request_t &req, response_t &res);
};

class AiAgentSessionListAction {
public:
	static bool run(request_t &req, response_t &res);
};

class AiAgentSessionSaveAction {
public:
	// Exports the bounded visible transcript and memory summary into its project.
	static bool run(request_t &req, response_t &res);
};

class AiAgentSessionDeleteAction {
public:
	static bool run(request_t &req, response_t &res);
};

class AiAgentProjectListAction {
public:
	// Lists project manifests, or returns one full plan when id/path is supplied.
	static bool run(request_t &req, response_t &res);
};

class AiAgentProjectDeleteAction {
public:
	// Removes AI metadata and child conversations, never virtual-disk files.
	static bool run(request_t &req, response_t &res);
};

class AiAgentProjectImportGitAction {
public:
	// Registers an existing virtual-disk Git workspace without modifying it.
	static bool run(request_t &req, response_t &res);
};

class AiAgentProjectEnsureAction {
public:
	// Registers an existing workspace path without modifying project files.
	static bool run(request_t &req, response_t &res);
};

class AiAgentProjectPlanSaveAction {
public:
	// Atomically replaces a versioned module graph and task DAG.
	static bool run(request_t &req, response_t &res);
};

class AiAgentProjectPlanProposeAction {
public:
	// Returns a deterministic proposal without modifying persisted project state.
	static bool run(request_t &req, response_t &res);
};

class AiAgentProjectTaskStatusAction {
public:
	static bool run(request_t &req, response_t &res);
};

class AiAgentProjectWorkflowGetAction {
public:
	static bool run(request_t &req, response_t &res);
};

class AiAgentProjectWorkflowSaveAction {
public:
	static bool run(request_t &req, response_t &res);
};

class AiAgentProjectIndexGetAction {
public:
	// Returns metadata only; source content remains behind workspace.read.
	static bool run(request_t &req, response_t &res);
};

class AiAgentProjectIndexRefreshAction {
public:
	static bool run(request_t &req, response_t &res);
};

} // namespace action
