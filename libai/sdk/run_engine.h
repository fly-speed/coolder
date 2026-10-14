#pragma once
// Internal synchronous loop, split into bounded request/execution phases.
#include "agent_types.h"
#include "durable_execution.h"

namespace libai
{
namespace sdk_detail
{

// One run owns its counters and cancellation boundary; no global state.
class run_engine_t {
public:
	// Borrow immutable configuration and the caller-owned mutable session.
	run_engine_t(model_t &model, const std::string &instructions,
	    const std::map<std::string, tool_t> &tools,
	    agent_session_t &session, const run_options_t &options)
	        : model_(model)
	        , instructions_(instructions)
	        , tools_(tools)
	        , session_(session)
	        , options_(options)
	{
		context_.cancellation = options.cancellation ?
		    options.cancellation :
		    std::make_shared<cancellation_t>();
        wall_started_=execution_wall_ms();
        timeout_valid_=options.timeout.count()>=0&&options.timeout.count()<=execution_timeout_max();
        context_.deadline = timeout_valid_&&options.timeout.count()?
            std::chrono::steady_clock::now()+options.timeout:std::chrono::steady_clock::time_point::max();
	}

	// Finish through a single event path, including application exceptions.
	run_result_t execute(const std::string &input)
	{
		try {
            if(!timeout_valid_){fail(run_status_t::callback_error,"timeout is out of range");return finish();}
			if (stopped())
				return finish();
			if (!options_.max_tokens_per_call ||
			    !options_.max_tool_result_bytes) {
				fail(run_status_t::budget_exceeded,
				    "invalid zero byte/token limit");
				return finish();
			}
            if(options_.max_output_repairs>8)throw std::invalid_argument("output repair feedback limit exceeds 8");
            if(!options_.output_schema.empty())output_schema_.reset(new json_schema_t(options_.output_schema));
            bool restored=false;
            if(options_.execution_store) {
                journal_.reset(new durable_checkpoint_t(options_.execution_store,options_.execution_scope,input,instructions_));
                restored=journal_->load(session_,result_);
                const auto max_repairs=output_schema_?options_.max_output_repairs:0;
                if(restored && (journal_->output_schema!=options_.output_schema || journal_->output_max_repairs!=max_repairs))
                    throw durable_error_t("frozen output contract changed");
                if(!restored){journal_->output_schema=options_.output_schema;journal_->output_max_repairs=max_repairs;}
                result_.output_repairs=journal_->output_repair_count;
                if(!journal_->output_failure.empty()){fail(run_status_t::protocol_error,journal_->output_failure);return finish();}
                result_.output_validated=output_schema_ && journal_->phase=="finished";
                for(std::size_t i=0;i<journal_->cursor;++i) {
                    call_ids_.insert(journal_->effects[i].call.id);
                    bool error=false;
                    for(auto message=session_.messages.rbegin();message!=session_.messages.rend();++message)
                        if(message->role==message_t::role_t::tool && message->call_id==journal_->effects[i].call.id){error=message->tool_error;break;}
                    observe_effect(agent_event_t::kind_t::tool_result_reused,journal_->effects[i],error);
                }
                if(restored && journal_->phase=="finished") {fail(run_status_t::completed,"");return finish();}
            } else if(!options_.execution_scope.empty()) throw durable_error_t("execution scope has no durable store");
            if(journal_) {
                // An unresolved effect must be reconciled before changing its checkpoint.
                if(journal_->running()&&!journal_->side_effect_free())throw durable_error_t("unconfirmed tool effect requires host reconciliation: "+journal_->key());
                if(restored&&!journal_->time_budget.present)throw durable_error_t("legacy unfinished checkpoint has no time budget; host deadline adoption required");
                if(!restored)journal_->time_budget.begin(wall_started_,options_.timeout.count());
                else journal_->time_budget.tighten(options_.timeout.count());
                context_.deadline=std::min(context_.deadline,journal_->time_budget.deadline());
                if(!restored){message_t user;user.text=input;session_.messages.push_back(user);}
                journal_->save(session_,result_); // Persist tightening before any resumed callback.
                if(stopped())return finish();
            }else if(!restored){message_t user;user.text=input;session_.messages.push_back(user);}
            if(journal_ && journal_->phase=="tools") {
                auto pending=journal_->pending(tools_);
                if(!validate_response(pending,0))return finish();
                execute_batch(pending.calls);
            }
			while (step()) {
			}
		} catch (const durable_error_t &exception) {
			fail(run_status_t::recovery_required, exception.what());
		} catch (const std::exception &exception) {
			fail(run_status_t::callback_error, exception.what());
		} catch (...) {
			fail(run_status_t::callback_error,
			    "model/event callback failed");
		}
		return finish();
	}

private:
    std::int64_t wall_started_=0;
    bool timeout_valid_=false,admission_skipped_=false;
    std::unique_ptr<durable_checkpoint_t> journal_;
    std::unique_ptr<json_schema_t> output_schema_;
    bool output_failure(const std::string &error) {
        if(journal_){journal_->output_failure=error;journal_->save(session_,result_);}
        return fail(run_status_t::protocol_error,error);
    }
	std::map<std::string,std::string> schema_errors_;
	std::size_t stream_remaining_ = 0;
	bool stream_limited_ = false;
	// Record a terminal result and provide an early-return condition.
	bool fail(run_status_t status, const std::string &error)
	{
		result_.status = status;
		result_.error = error;
		return false;
	}
	// Observe cancellation before deadlines and before each execution phase.
	bool stopped()
	{
        bool wall_expired=journal_&&journal_->time_budget.expired();
		if (!context_.stopped()&&!wall_expired)
			return false;
		fail(context_.cancellation->cancelled() ?
		        run_status_t::cancelled :
		        run_status_t::deadline_exceeded,
		    "run stopped");
		return true;
	}
	// Notify observers on the run thread; phase callers handle exceptions.
	void emit(const agent_event_t &event)
	{
        if (!options_.on_event) return;
        auto observed=event;observed.execution_scope=options_.execution_scope;
        if(journal_) {
            observed.journal_revision=journal_->revision();
            if(observed.idempotency_key.empty() && !observed.call_id.empty() && journal_->cursor<journal_->effects.size()) {
                const auto &effect=journal_->effects[journal_->cursor];
                if(effect.call.id==observed.call_id) {
                    observed.idempotency_key=effect.key;observed.effect_state=effect.state;observed.attempts=effect.attempts;
                }
            }
        }
        options_.on_event(observed);
	}
    // Commit/reuse observations are presentation only; they cannot undo storage
    // or authorize a handler. Cancellation is still checked at dispatch boundaries.
    void observe_effect(agent_event_t::kind_t kind,const durable_checkpoint_t::effect_t &effect,bool error=false) {
        agent_event_t event(kind);event.tool_name=effect.call.name;event.call_id=effect.call.id;
        event.idempotency_key=effect.key;event.effect_state=effect.state;event.attempts=effect.attempts;event.error=error;
        try{emit(event);}catch(...){}
    }
	// Always return a result even if a terminal observer fails.
	run_result_t finish()
	{
		agent_event_t event(agent_event_t::kind_t::finished);
		event.error = result_.status != run_status_t::completed;
		try {
			emit(event);
		} catch (const std::exception &exception) {
			fail(run_status_t::callback_error, exception.what());
		} catch (...) {
			fail(run_status_t::callback_error,
			    "finished event callback failed");
		}
		const char *outcomes[] = { "completed", "cancelled", "deadline_exceeded",
            "budget_exceeded", "model_error", "protocol_error", "callback_error", "recovery_required" };
        result_.task_contract.loop_finished = true;
        result_.task_contract.loop_outcome = outcomes[static_cast<unsigned>(result_.status)];
		return result_;
	}
	// Bound reported counters even for a defective custom model backend.
	static std::size_t add_usage(std::size_t total, std::size_t value)
	{
		const auto maximum = std::numeric_limits<std::size_t>::max();
		return value > maximum - total ? maximum : total + value;
	}
	// Count opaque argument bytes without parsing application-owned JSON.
	static std::size_t call_bytes(const tool_call_t &call)
	{
		return call.id.size() + call.name.size() +
		    call.arguments.size();
	}
	// Include reasoning replay and call identifiers in the context budget.
	static std::size_t message_bytes(const message_t &message)
	{
		std::size_t bytes = message.text.size() +
		    message.call_id.size() + message.reasoning.size();
		for (const auto &call : message.calls)
			bytes += call_bytes(call);

		return bytes;
	}
	// Snapshot the typed transcript and advertise the current tool catalog.
	std::size_t make_request(model_request_t &request)
	{
		request.profile = options_.profile;
		request.instructions = instructions_;
        request.output_schema=options_.output_schema;
        request.output_repair=result_.output_repairs>0;
        if(output_schema_)request.instructions+="\nReturn the final answer as raw JSON matching this Draft-04 schema: "+options_.output_schema;
        if(request.output_repair)request.instructions+="\nCorrect only the final JSON. Do not call tools or repeat prior work.";
		request.messages = session_.messages;
		request.cancellation = context_.cancellation;
		request.deadline = context_.deadline;
		request.max_output_tokens =
		    std::min(options_.max_tokens_per_call,
		        options_.max_output_tokens - result_.output_tokens);
		std::size_t bytes = request.instructions.size() + request.output_schema.size() + request.profile.json.size();
		for (const auto &message : session_.messages)
			bytes += message_bytes(message);
		for (const auto &entry : tools_) {
			if (!options_.profile.tool_calling || request.output_repair) break;
			const auto &spec = entry.second.spec;
			request.tools.push_back(spec);
			bytes += spec.name.size() + spec.description.size() +
			    spec.parameters.size();
		}
		return bytes;
	}
	// Stream cancellation propagates to the model and later tool phases.
	bool text_delta(const std::string &delta)
	{
		if (context_.stopped())
			return false;
        if (delta.size() > stream_remaining_) {
            stream_limited_ = true;
            return false;
        }
        stream_remaining_ -= delta.size();
		if (!output_schema_ && options_.on_text && !options_.on_text(delta)) {
			context_.cancellation->cancel();
			return false;
		}
		return true;
	}
	// Count every logical attempt, including recoverable model failures.
	model_response_t call_model(const model_request_t &request)
	{
		emit(agent_event_t(agent_event_t::kind_t::model_start));
		++result_.model_calls;
        if(journal_)journal_->save(session_,result_);
        model_response_t response;
        if(stopped())response.error="run stopped before model dispatch";
        else response=model_.complete(request,[this](const std::string &delta){return text_delta(delta);});
		result_.input_tokens =
		    add_usage(result_.input_tokens, response.input_tokens);
		result_.output_tokens =
		    add_usage(result_.output_tokens, response.output_tokens);
        if(journal_)journal_->save(session_,result_);
        agent_event_t ended(agent_event_t::kind_t::model_end);
        ended.error = !response.error.empty() || stream_limited_;
        emit(ended);
		return response;
	}
	// Validate the complete batch before any application tool has side effects.
	bool validate_response(
	    const model_response_t &response, std::size_t request_bytes)
	{
		if (result_.output_tokens > options_.max_output_tokens ||
		    response.output_tokens > options_.profile.max_output_tokens)
			return fail(run_status_t::budget_exceeded,
			    "provider exceeded token budget");
        if ((!options_.profile.tool_calling && !response.calls.empty()) ||
            (!options_.profile.multiple_tool_calls && response.calls.size()>1))
            return fail(run_status_t::protocol_error, "model response violates declared tool capabilities");
        schema_errors_.clear();
        if(result_.tool_calls>options_.max_tool_calls)
            return fail(run_status_t::budget_exceeded,"restored tool usage exceeds current budget");
        const auto remaining=options_.max_tool_calls-result_.tool_calls;
        // Preserve protocol-error priority in the bounded batch prefix.
        const auto inspect=std::min(response.calls.size(), remaining==std::numeric_limits<std::size_t>::max()?remaining:remaining+1);
        std::size_t bytes = response.text.size() + response.reasoning.size();
        for (std::size_t i=0;i<inspect;++i) {
            const auto &call=response.calls[i];
            bytes += call_bytes(call);
            if (call.id.empty() || call.name.empty() || !call_ids_.insert(call.id).second)
                return fail(run_status_t::protocol_error, "empty or duplicate tool call identifier");
        }
        if (response.calls.size()>remaining)
            return fail(run_status_t::budget_exceeded,"tool batch exceeds remaining budget");
        if (bytes > options_.max_context_bytes - request_bytes)
            return fail(run_status_t::budget_exceeded, "model response exceeds context byte budget");
        // Only parse arguments after cheap count/byte checks have passed.
        for (const auto &call : response.calls) {
            auto tool=tools_.find(call.name);
            if (tool==tools_.end()) schema_errors_[call.id]="unknown tool";
            else {
                auto validation=tool->second.schema->validate(call.arguments);
                if (!validation.valid)
                    schema_errors_[call.id]="tool arguments rejected: "+validation.code+
                        " "+validation.keyword+" "+validation.instance_pointer;
            }
        }
		return true;
	}
	// Advance one model turn; false means a terminal state has been recorded.
	bool step()
	{
		if (stopped())
			return false;
		if (result_.model_calls >= options_.max_model_calls ||
		    result_.output_tokens >= options_.max_output_tokens)
			return fail(run_status_t::budget_exceeded,
			    "model/token budget exhausted");
		model_request_t request;
		const auto bytes = make_request(request);
		if (bytes > options_.max_context_bytes)
			return fail(run_status_t::budget_exceeded,
			    "context byte budget exhausted");
        stream_remaining_ = options_.max_context_bytes - bytes;
        stream_limited_ = false;
		auto response = call_model(request);
        if (stream_limited_)
            return fail(run_status_t::budget_exceeded, "streamed text exceeds context byte budget");
		if (stopped())
			return false;
		if (!response.error.empty()) {
			if (response.retryable &&
			    retries_++ < options_.max_model_retries)
				return true;
			return fail(run_status_t::model_error, response.error);
		}
		retries_ = 0;
        if(result_.output_repairs && !response.calls.empty())
            return output_failure("structured output repair attempted tool calls");
		if (!validate_response(response, bytes))
			return false;
		message_t assistant;
		assistant.role = message_t::role_t::assistant;
		assistant.text = response.text;
		assistant.reasoning = response.reasoning;
		assistant.calls = response.calls;
		session_.messages.push_back(assistant);
		if (response.calls.empty()) {
            if(output_schema_) {
                const auto validation=output_schema_->validate(response.text);
                if(!validation.valid) {
                    const auto error="structured output invalid: "+validation.code+" "+validation.keyword+" "+validation.instance_pointer;
                    if(result_.output_repairs>=options_.max_output_repairs)return output_failure(error);
                    ++result_.output_repairs;
                    message_t feedback;feedback.role=message_t::role_t::user;
                    feedback.text=std::string(durable_checkpoint_t::feedback_prefix())+error+". Return corrected JSON only; tools are forbidden.";
                    session_.messages.push_back(feedback);
                    if(journal_){journal_->output_repair_count=result_.output_repairs;journal_->save(session_,result_);}
                    return true;
                }
                result_.output_validated=true;
            }
			result_.text = response.text;
            if(journal_){journal_->phase="finished";journal_->save(session_,result_);}
            if(output_schema_ && options_.on_text && !options_.on_text(response.text)) {
                context_.cancellation->cancel();return fail(run_status_t::cancelled,"validated output delivery cancelled");
            }
			return fail(run_status_t::completed, "");
		}
		// Ordered execution preserves dependencies between arbitrary mutations.
        if(journal_){journal_->prepare(response.calls,tools_);journal_->save(session_,result_);}
        execute_batch(response.calls);
		return true;
	}
    void execute_batch(const std::vector<tool_call_t> &calls) {
        for(const auto &call:calls) {
            admission_skipped_=false;
            auto output=execute_tool(call);
            if(journal_ && journal_->running() && output.error && !journal_->side_effect_free() && !admission_skipped_) {
                agent_event_t event(agent_event_t::kind_t::tool_end);event.tool_name=call.name;event.call_id=call.id;event.error=true;
                try{emit(event);}catch(...){}
                throw durable_error_t("tool failed after admission; side effect outcome requires reconciliation: "+journal_->key());
            }
            append_result(call,output);
            if(journal_) {
                journal_->done(admission_skipped_?"deadline/cancelled before handler dispatch":output.error?(journal_->running()?"tool returned error without side effects":"not executed"):"tool returned success");
                journal_->save(session_,result_);
                observe_effect(agent_event_t::kind_t::tool_result_committed,journal_->effects[journal_->cursor-1],session_.messages.back().tool_error);
            }
        }
    }
	// Authorization may cancel a run, so check the boundary again afterward.
	tool_result_t dispatch_tool(const tool_call_t &call)
	{
		if (stopped())
			return tool_result_t::failure(
			    "run stopped; tool skipped");
        if (!schema_errors_.empty()) {
            auto error=schema_errors_.find(call.id);
            return tool_result_t::failure(error==schema_errors_.end()?
                "tool batch rejected because another call failed validation":error->second);
        }
		const auto found = tools_.find(call.name);
		if (found == tools_.end())
			return tool_result_t::failure("unknown tool");
		const auto &tool = found->second;
		const bool allowed = options_.authorize ?
		    options_.authorize(tool.spec, call) :
		    !tool.spec.requires_approval;
		if (!allowed)
			return tool_result_t::failure(
			    "tool authorization denied");
		if (stopped())
			return tool_result_t::failure(
			    "run stopped; tool skipped");
		++result_.tool_calls;
        auto context=context_;
        if(journal_){context.idempotency_key=journal_->key();journal_->start();journal_->save(session_,result_);
            observe_effect(agent_event_t::kind_t::tool_intent_committed,journal_->effects[journal_->cursor]);}
        if(stopped()){admission_skipped_=true;return tool_result_t::failure("run stopped after intent commit; handler not invoked");}
		return tool.execute(call, context);
	}
	// Convert tool/authorization/start-observer exceptions into paired results.
	tool_result_t execute_tool(const tool_call_t &call)
	{
		try {
			agent_event_t event(agent_event_t::kind_t::tool_start);
			event.tool_name = call.name;
			event.call_id = call.id;
			emit(event);
			return dispatch_tool(call);
		} catch (const durable_error_t &) { throw;
		} catch (const std::exception &exception) {
			return tool_result_t::failure(exception.what());
		} catch (...) {
			return tool_result_t::failure("tool callback failed");
		}
	}
	// Preserve UTF-8 boundaries and close every exchange, including skipped calls.
	void append_result(const tool_call_t &call, tool_result_t output)
	{
		if (output.text.size() > options_.max_tool_result_bytes) {
			std::size_t end = options_.max_tool_result_bytes;
			while (end &&
			    (static_cast<unsigned char>(output.text[end]) &
			        0xc0) == 0x80)
				--end;
			output.text.resize(end);
			output.error = true;
		}
		message_t message;
		message.role = message_t::role_t::tool;
		message.call_id = call.id;
		message.text = output.text;
		message.tool_error = output.error;
		session_.messages.push_back(message);
		agent_event_t event(agent_event_t::kind_t::tool_end);
		event.tool_name = call.name;
		event.call_id = call.id;
		event.error = output.error;
		// A presentation observer cannot prevent the remaining results pairing.
		try {
			emit(event);
		} catch (...) {
		}
	}

	// Application model and immutable configuration are borrowed for this run.
	model_t &model_;
	// Stable instructions copied into each provider request.
	const std::string &instructions_;
	// Registered application capabilities and their handlers.
	const std::map<std::string, tool_t> &tools_;
	// Caller-owned transcript retains partial side effects on cancellation.
	agent_session_t &session_;
	// Observer, authorization and budget settings for the current run.
	const run_options_t &options_;
	// Cooperative cancellation and deadline exposed to tool handlers.
	tool_context_t context_;
	// Final status and counters accumulated across attempts.
	run_result_t result_;
	// Reject repeated call IDs before executing a new batch.
	std::set<std::string> call_ids_;
	// Consecutive recoverable model failures in the current logical turn.
	std::size_t retries_ = 0;
};
} // namespace sdk_detail
} // namespace libai
