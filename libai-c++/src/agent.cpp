#include <libai_cpp/agent.h>
#include "value_bridge.h"

namespace libai
{
namespace cpp
{
// A failed CAS rejects both concurrent and callback-reentrant access safely.
class StateGuard {
public:
	// Claim exclusive access without locking recursively on the run thread.
	explicit StateGuard(std::atomic<bool> &busy)
	        : busy_(busy)
	{
		bool expected = false;
		if (!busy_.compare_exchange_strong(expected, true))
			throw std::logic_error("agent is busy");
	}
	// Release exclusive access even when a callback or allocation fails.
	~StateGuard()
	{
		busy_.store(false);
	}
	// Each successful claim must be released exactly once.
	StateGuard(const StateGuard &) = delete;
	// State guards cannot transfer another owner's release obligation.
	StateGuard &operator=(const StateGuard &) = delete;

private:
	// Atomic ownership shared by state operations on the facade.
	std::atomic<bool> &busy_;
};
Cancellation::Cancellation()
        : impl_(new Impl)
{
}
Cancellation::~Cancellation() = default;
void Cancellation::cancel() noexcept
{
	impl_->native->cancel();
}
bool Cancellation::cancelled() const noexcept
{
	return impl_->native->cancelled();
}

// Translate an application transport at the implementation boundary.
class ModelAdapter : public libai::model_t {
public:
	// Retain the independent model and the currently active public token.
	ModelAdapter(std::shared_ptr<Model> model,
	    std::shared_ptr<Cancellation> &cancellation)
	        : model_(std::move(model))
	        , cancellation_(cancellation)
	{
		if (!model_)
			throw std::invalid_argument("model is null");
	}
	// Copy requests and responses without native types crossing the callback boundary.
	libai::model_response_t complete(const libai::model_request_t &source,
	    const libai::stream_sink_t &sink) override
	{
		ModelRequest request;
		request.instructions = source.instructions;
		request.messages = detail::convert_values<Message>(
		    source.messages, detail::message_value);
		request.tools = detail::convert_values<ToolSpec>(source.tools,
		    detail::spec_value<ToolSpec, libai::tool_spec_t>);
		request.max_output_tokens = source.max_output_tokens;
		request.cancellation = std::atomic_load(&cancellation_);
		request.deadline = source.deadline;
		return detail::response_value(model_->complete(request, sink));
	}

private:
	// Public transport stays alive until the owning agent is destroyed.
	std::shared_ptr<Model> model_;
	// Stable storage owned by Agent::Impl; only the run thread writes requests.
	std::shared_ptr<Cancellation> &cancellation_;
};
// Adapt the independent function interface into the independent model interface.
class FunctionModel : public Model {
public:
	// Reject empty callbacks before constructing a live agent.
	explicit FunctionModel(ModelFunction function)
	        : function_(std::move(function))
	{
		if (!function_)
			throw std::invalid_argument("model callback is empty");
	}
	// Forward the facade request to the application callback.
	ModelResponse complete(
	    const ModelRequest &request, const StreamSink &sink) override
	{
		return function_(request, sink);
	}

private:
	// Application transport retained for the model lifetime.
	ModelFunction function_;
};
// Serialize transcript/configuration while allowing independent cancellation.
struct Agent::Impl {
	// Construct the runtime only after the shared token slot has been initialized.
	Impl(std::shared_ptr<Model> model, const std::string &instructions)
	        : runtime(std::make_shared<ModelAdapter>(
	                      std::move(model), cancellation),
	              instructions)
	{
	}
	// Atomic ownership serializes configuration and the transcript.
	std::atomic<bool> busy{ false };
	// Public token supplied unchanged to transport and tool callbacks.
	std::shared_ptr<Cancellation> cancellation;
	// Generic SDK loop and conversation remain private to the compiled facade.
	libai::agent_t runtime;
	// Typed native transcript translated when exposed to the application.
	libai::agent_session_t session;
};
Agent::Agent(std::shared_ptr<Model> model, const std::string &instructions)
        : impl_(new Impl(std::move(model), instructions))
{
}
Agent::Agent(ModelFunction model, const std::string &instructions)
        : Agent(std::make_shared<FunctionModel>(std::move(model)), instructions)
{
}
Agent::~Agent() = default;

Agent &Agent::add_tool(const ToolSpec &spec, ToolHandler handler)
{
	StateGuard guard(impl_->busy);
	if (!handler)
		throw std::invalid_argument("tool handler is empty");
	impl_->runtime.add_tool(detail::spec_value<libai::tool_spec_t>(spec),
	    [this, handler](const libai::tool_call_t &call,
	        const libai::tool_context_t &context) {
		ToolContext public_context;
		public_context.cancellation =
		    std::atomic_load(&impl_->cancellation);
		public_context.deadline = context.deadline;
		auto result =
		    handler(detail::call_value<ToolCall>(call), public_context);
		libai::tool_result_t native;
		native.text = result.text;
		native.error = result.error;
		return native;
	});
	return *this;
}
RunResult Agent::run(const std::string &input, const RunOptions &options)
{
	StateGuard guard(impl_->busy);
	const auto maximum =
	    std::chrono::duration_cast<std::chrono::milliseconds>(
	        std::chrono::steady_clock::duration::max())
	        .count() /
	    2;
	if (options.timeout.count() < 0 || options.timeout.count() > maximum)
		throw std::invalid_argument("timeout is out of range");
	auto cancellation = options.cancellation ?
	    options.cancellation :
	    std::make_shared<Cancellation>();
	std::atomic_store(&impl_->cancellation, cancellation);
	libai::run_options_t native;
	native.max_model_calls = options.max_model_calls;
	native.max_tool_calls = options.max_tool_calls;
	native.max_output_tokens = options.max_output_tokens;
	native.max_tokens_per_call = options.max_tokens_per_call;
	native.max_context_bytes = options.max_context_bytes;
	native.max_tool_result_bytes = options.max_tool_result_bytes;
	native.max_model_retries = options.max_model_retries;
	native.timeout = options.timeout;
	native.cancellation = cancellation->impl_->native;
	native.on_text = options.on_text;
	if (options.authorize)
		native.authorize = [options](const libai::tool_spec_t &spec,
		                       const libai::tool_call_t &call) {
			return options.authorize(
			    detail::spec_value<ToolSpec>(spec),
			    detail::call_value<ToolCall>(call));
		};
	if (options.on_event)
		native.on_event = [options](const libai::agent_event_t &event) {
			options.on_event(detail::event_value(event));
		};
	return detail::result_value(
	    impl_->runtime.run(impl_->session, input, native));
}
void Agent::cancel() noexcept
{
	auto cancellation = std::atomic_load(&impl_->cancellation);
	if (cancellation)
		cancellation->cancel();
}
void Agent::reset()
{
	StateGuard guard(impl_->busy);
	impl_->session.messages.clear();
}
std::vector<Message> Agent::messages() const
{
	StateGuard guard(impl_->busy);
	return detail::convert_values<Message>(
	    impl_->session.messages, detail::message_value);
}

} // namespace cpp
} // namespace libai
