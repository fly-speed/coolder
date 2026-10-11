#pragma once
#include "types.h"

namespace libai
{
namespace cpp
{
// Owning C++ facade with a private native runtime and persistent session.
// Concurrent runs/configuration are rejected; cancel may be called concurrently.
class LIBAI_CPP_API Agent {
public:
	// Bind an application model implementation and stable system instructions.
	explicit Agent(
	    std::shared_ptr<Model> model, const std::string &instructions = "");
	// Adapt an application callback without requiring a model subclass.
	explicit Agent(
	    ModelFunction model, const std::string &instructions = "");
	// Destroy after all run/cancel calls have completed; never from a callback.
	~Agent();
	// Agents own callback/session state and cannot be copied.
	Agent(const Agent &) = delete;
	// Prevent assigning a session owner while another thread may use it.
	Agent &operator=(const Agent &) = delete;
	// Keep the facade address stable during application callbacks.
	Agent(Agent &&) = delete;
	// Prevent moving live native state between application objects.
	Agent &operator=(Agent &&) = delete;
	// Register one uniquely named application capability while idle.
	Agent &add_tool(const ToolSpec &spec, ToolHandler handler);
	// Run one bounded turn, retaining the owned transcript for later turns.
	RunResult run(
	    const std::string &input, const RunOptions &options = RunOptions());
	// Request cooperative cancellation of the current run from any thread.
	void cancel() noexcept;
	// Clear an idle session while retaining the model and registered tools.
	void reset();
	// Copy the idle transcript for application presentation or persistence.
	std::vector<Message> messages() const;

private:
	// Implementation details stay inside the compiled library.
	struct Impl;
	// Unique native runtime, session and synchronization state.
	std::unique_ptr<Impl> impl_;
};

} // namespace cpp
} // namespace libai
