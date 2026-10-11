#pragma once
#include "agent.h"
#include "provider_config.h"
namespace libai
{
namespace cpp
{
// Native provider transport that accepts only independently declared facade types.
class LIBAI_CPP_API NativeProviderModel : public Model {
public:
	// Retain provider configuration and a credential without exposing native clients.
	NativeProviderModel(
	    const ProviderConfig &config, const std::string &api_key);
	// Release provider state when no requests remain active.
	~NativeProviderModel() override;
	// Provider instances retain request sequencing and are not copied.
	NativeProviderModel(const NativeProviderModel &) = delete;
	// Prevent assigning live transport state.
	NativeProviderModel &operator=(const NativeProviderModel &) = delete;
	// Complete a facade request using the private native transport implementation.
	ModelResponse complete(
	    const ModelRequest &request, const StreamSink &sink) override;

private:
	// Hide provider client and credentials behind an implementation boundary.
	struct Impl;
	// Unique provider transport implementation.
	std::unique_ptr<Impl> impl_;
};
}
}
