#pragma once

namespace action
{

// HTTP adapters around the administrator-owned encrypted provider store.
// Listing remains available to authenticated users so enabled system models can
// be selected for agent runs; all mutations and connection tests require admin.
// No action returns plaintext or encrypted API key material to the browser.
class AiProviderListAction {
public:
	static bool run(request_t &req, response_t &res);
};

class AiProviderSaveAction {
public:
	static bool run(request_t &req, response_t &res);
};

class AiProviderDeleteAction {
public:
	static bool run(request_t &req, response_t &res);
};

class AiProviderTestAction {
public:
	static bool run(request_t &req, response_t &res);
};

class AiAgentTypesAction {
public:
	static bool run(request_t &req, response_t &res);
};

} // namespace action
