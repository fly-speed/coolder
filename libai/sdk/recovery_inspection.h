#pragma once
#include "durable_execution.h"

namespace libai { namespace sdk_detail {
// An immutable copy, never an adapter to the application's live store.
class inspection_store_t final : public execution_store_t {
    execution_snapshot_t snapshot_;
public:
    explicit inspection_store_t(execution_snapshot_t snapshot):snapshot_(std::move(snapshot)){}
    execution_snapshot_t open(const std::string &) override {return snapshot_;}
    std::string commit(const std::string &,const std::string &,const std::string &) override {
        throw std::logic_error("inspection cannot commit");
    }
    void close(const std::string &) noexcept override {}
};
}}
namespace libai {
// Trusted host diagnostic. A snapshot is not authorization or an external receipt.
inline std::string inspect_execution_json(const std::string &arguments) {
    namespace json=governance_detail::json;
    try {
        json::Document args;governance_detail::parse_json(arguments,args);
        if(!args.IsObject())throw std::invalid_argument("inspection arguments must be an object");
        auto field=[&](const char *name)->std::string {
            if(!args.HasMember(name)||!args[name].IsString())throw std::invalid_argument("inspection requires scope, revision, state_json, input and instructions strings");
            return std::string(args[name].GetString(),args[name].GetStringLength());
        };
        execution_snapshot_t snapshot;snapshot.revision=field("revision");snapshot.state_json=field("state_json");
        auto store=std::make_shared<sdk_detail::inspection_store_t>(snapshot);
        sdk_detail::durable_checkpoint_t checkpoint(store,field("scope"),field("input"),field("instructions"));
        agent_session_t session;run_result_t run;
        // Inspect clocks without changing last_seen or rejecting historical snapshots.
        const bool exists=checkpoint.load(session,run,false);
        const auto &time=checkpoint.time_budget;
        const auto now=sdk_detail::execution_wall_ms();
        const bool backwards=time.present&&(now<time.last_seen||now<time.started);
        const bool expired=time.present&&time.timeout_ms&&now>=time.started+time.timeout_ms;
        bool unknown=false;
        for(const auto &e:checkpoint.effects)if(e.state=="running"&&!e.side_effect_free)unknown=true;
        const char *recovery=!exists?"new_task":unknown?"host_reconciliation":
            backwards?"clock_reconciliation":checkpoint.phase=="finished"?"cached_result":
            !checkpoint.output_failure.empty()?"cached_output_failure":!time.present?"deadline_adoption":
            expired?"deadline_exceeded":"resume_pending";
        json::StringBuffer buffer;json::Writer<json::StringBuffer> out(buffer);
        auto text=[&](const char *key,const std::string &value){out.Key(key);out.String(value.data(),static_cast<json::SizeType>(value.size()));};
        out.StartObject();out.Key("ok");out.Bool(true);out.Key("version");out.Uint(1);
        out.Key("has_checkpoint");out.Bool(exists);text("revision",snapshot.revision);text("recovery",recovery);
        text("phase",exists?checkpoint.phase:"");out.Key("cursor");out.Uint64(checkpoint.cursor);
        out.Key("model_calls");out.Uint64(run.model_calls);out.Key("tool_calls");out.Uint64(run.tool_calls);
        out.Key("clock_moved_backwards");out.Bool(backwards);out.Key("deadline_expired");out.Bool(expired);
        out.Key("time_budget_present");out.Bool(time.present);
        out.Key("effects");out.StartArray();
        for(const auto &e:checkpoint.effects){
            out.StartObject();text("key",e.key);text("call_id",e.call.id);text("name",e.call.name);
            text("arguments_json",e.call.arguments);text("execution_revision",e.revision);text("state",e.state);
            text("evidence",e.evidence);out.Key("attempts");out.Uint64(e.attempts);
            out.Key("side_effect_free");out.Bool(e.side_effect_free);
            text("resume_policy",e.state=="done"?"reuse_result":e.state=="ready"?"validate_and_authorize":
                e.side_effect_free?"validate_and_authorize_replay":"host_receipt_required");out.EndObject();
        }
        out.EndArray();out.EndObject();return std::string(buffer.GetString(),buffer.GetSize());
    }catch(const std::exception &e){
        json::StringBuffer buffer;json::Writer<json::StringBuffer> out(buffer);out.StartObject();
        out.Key("ok");out.Bool(false);out.Key("error");out.String(e.what());out.EndObject();
        return std::string(buffer.GetString(),buffer.GetSize());
    }
}
}
