#pragma once

namespace action
{

// HTTP adapters around the administrator-owned encrypted provider store.
// Listing remains available to authenticated users so enabled system models can
// be selected for agent runs; all mutations and connection tests require admin.
// No action returns plaintext or encrypted API key material to the browser.
class AiProviderListAction {
public:
	// Handle the HTTP request and write the corresponding JSON response.
	static bool run(request_t &req, response_t &res);
};

// HTTP handler for AI provider save.
class AiProviderSaveAction {
public:
	// Handle the HTTP request and write the corresponding JSON response.
	static bool run(request_t &req, response_t &res);
};

// HTTP handler for AI provider delete.
class AiProviderDeleteAction {
public:
	// Handle the HTTP request and write the corresponding JSON response.
	static bool run(request_t &req, response_t &res);
};

// HTTP handler for AI provider test.
class AiProviderTestAction {
public:
	// Handle the HTTP request and write the corresponding JSON response.
	static bool run(request_t &req, response_t &res);
};

// HTTP handler for AI agent types.
class AiAgentTypesAction {
public:
	// Handle the HTTP request and write the corresponding JSON response.
	static bool run(request_t &req, response_t &res);
};

} // namespace action
