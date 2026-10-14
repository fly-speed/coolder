#pragma once
#include "types.h"
#include "governance.h"

namespace libai
{
namespace cpp
{
class McpClient;
struct McpBinding;
class Retriever;
struct RagBinding;
// Last returned run's schema validation; independent of artifact acceptance.
struct OutputValidation { bool output_validated=false; std::size_t output_repairs=0; };
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
	// Configure versioned JSON capabilities while idle; never exposes native types.
	Agent &configure_model(const std::string &profile_json);
    // Host-validate final JSON; bounded correction never executes additional tools.
    Agent &configure_output(const std::string &schema, std::size_t repairs = 1);
    // Idle snapshot; preserves the existing RunResult value ABI.
    OutputValidation output_validation() const;
    // Only on this Agent's on_event callback thread; returns an owning snapshot.
    EventExecution event_execution() const;
	// Register one uniquely named application capability while idle.
	Agent &add_tool(const ToolSpec &spec, ToolHandler handler);
    // Bind retrieval using host-owned scope/filter/revision and normal tool governance.
    Agent &add_retrieval_tool(std::shared_ptr<Retriever>,const std::string &name,const RagBinding &);
	// Bind a discovered MCP tool using explicit host policy, while idle.
	Agent &add_mcp_tool(std::shared_ptr<McpClient> client,const std::string &remote_name,
	    const std::string &local_name,const McpBinding &policy);
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
