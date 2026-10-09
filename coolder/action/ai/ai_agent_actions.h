#pragma once

namespace action
{

// Starts an asynchronous model/tool loop and returns a run ID immediately.
class AiAgentRunStartAction {
public:
	// Handle the HTTP request and write the corresponding JSON response.
	static bool run(request_t &req, response_t &res);
};

// HTTP handler for AI agent run list.
class AiAgentRunListAction {
public:
	// Handle the HTTP request and write the corresponding JSON response.
	static bool run(request_t &req, response_t &res);
};

// HTTP handler for AI agent run status.
class AiAgentRunStatusAction {
public:
	// Handle the HTTP request and write the corresponding JSON response.
	static bool run(request_t &req, response_t &res);
};

// HTTP handler for AI agent run events.
class AiAgentRunEventsAction {
public:
	// Authenticated, per-user SSE stream containing metadata-only progress.
	static bool run(request_t &req, response_t &res);
};

// HTTP handler for AI agent run cancel.
class AiAgentRunCancelAction {
public:
	// Cancellation is cooperative at provider/tool boundaries.
	static bool run(request_t &req, response_t &res);
};

// HTTP handler for AI agent run pause.
class AiAgentRunPauseAction {
public:
	// Pauses at the next safe provider/tool boundary and preserves all state.
	static bool run(request_t &req, response_t &res);
};

// HTTP handler for AI agent run result decision.
class AiAgentRunResultDecisionAction {
public:
	// Records accept/reject without deleting the project-local generated result.
	static bool run(request_t &req, response_t &res);
};

// HTTP handler for AI agent run result review.
class AiAgentRunResultReviewAction {
public:
	// Persists accepted/rejected state for exact file generations.
	static bool run(request_t &req, response_t &res);
};

// HTTP handler for AI agent run result apply.
class AiAgentRunResultApplyAction {
public:
	// Recovers a persisted result whose atomic source write was interrupted.
	static bool run(request_t &req, response_t &res);
};

// HTTP handler for AI agent run reasoning save.
class AiAgentRunReasoningSaveAction {
public:
	// Saves the current reasoning snapshot to a fixed .txt file in its project.
	static bool run(request_t &req, response_t &res);
};

// HTTP handler for AI assistant session list.
class AiAssistantSessionListAction {
public:
	// Handle the HTTP request and write the corresponding JSON response.
	static bool run(request_t &req, response_t &res);
};

// HTTP handler for AI assistant image.
class AiAssistantImageAction {
public:
	// Handle the HTTP request and write the corresponding JSON response.
	static bool run(request_t &req, response_t &res);
};

// HTTP handler for AI assistant session tag.
class AiAssistantSessionTagAction {
public:
	// Handle the HTTP request and write the corresponding JSON response.
	static bool run(request_t &req, response_t &res);
};

// HTTP handler for AI assistant session update.
class AiAssistantSessionUpdateAction {
public:
	// Handle the HTTP request and write the corresponding JSON response.
	static bool run(request_t &req, response_t &res);
};

// HTTP handler for AI assistant session import.
class AiAssistantSessionImportAction {
public:
	// Handle the HTTP request and write the corresponding JSON response.
	static bool run(request_t &req, response_t &res);
};

// HTTP handler for AI agent session list.
class AiAgentSessionListAction {
public:
	// Handle the HTTP request and write the corresponding JSON response.
	static bool run(request_t &req, response_t &res);
};

// HTTP handler for AI agent session save.
class AiAgentSessionSaveAction {
public:
	// Exports the bounded visible transcript and memory summary into its project.
	static bool run(request_t &req, response_t &res);
};

// HTTP handler for AI agent session delete.
class AiAgentSessionDeleteAction {
public:
	// Handle the HTTP request and write the corresponding JSON response.
	static bool run(request_t &req, response_t &res);
};

// HTTP handler for AI agent project list.
class AiAgentProjectListAction {
public:
	// Lists project manifests, or returns one full plan when id/path is supplied.
	static bool run(request_t &req, response_t &res);
};

// HTTP handler for AI agent project delete.
class AiAgentProjectDeleteAction {
public:
	// Removes AI metadata and child conversations, never virtual-disk files.
	static bool run(request_t &req, response_t &res);
};

// HTTP handler for AI agent project import git.
class AiAgentProjectImportGitAction {
public:
	// Registers an existing virtual-disk Git workspace without modifying it.
	static bool run(request_t &req, response_t &res);
};

// HTTP handler for AI agent project ensure.
class AiAgentProjectEnsureAction {
public:
	// Registers an existing workspace path without modifying project files.
	static bool run(request_t &req, response_t &res);
};

// HTTP handler for AI agent project plan save.
class AiAgentProjectPlanSaveAction {
public:
	// Atomically replaces a versioned module graph and task DAG.
	static bool run(request_t &req, response_t &res);
};

// HTTP handler for AI agent project plan propose.
class AiAgentProjectPlanProposeAction {
public:
	// Returns a deterministic proposal without modifying persisted project state.
	static bool run(request_t &req, response_t &res);
};

// HTTP handler for AI agent project task status.
class AiAgentProjectTaskStatusAction {
public:
	// Handle the HTTP request and write the corresponding JSON response.
	static bool run(request_t &req, response_t &res);
};

// HTTP handler for AI agent project workflow get.
class AiAgentProjectWorkflowGetAction {
public:
	// Handle the HTTP request and write the corresponding JSON response.
	static bool run(request_t &req, response_t &res);
};

// HTTP handler for AI agent project workflow save.
class AiAgentProjectWorkflowSaveAction {
public:
	// Handle the HTTP request and write the corresponding JSON response.
	static bool run(request_t &req, response_t &res);
};

// HTTP handler for AI agent project index get.
class AiAgentProjectIndexGetAction {
public:
	// Returns metadata only; source content remains behind workspace.read.
	static bool run(request_t &req, response_t &res);
};

// HTTP handler for AI agent project index refresh.
class AiAgentProjectIndexRefreshAction {
public:
	// Handle the HTTP request and write the corresponding JSON response.
	static bool run(request_t &req, response_t &res);
};

} // namespace action
