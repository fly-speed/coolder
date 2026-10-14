#pragma once
#include "agent_types.h"
#include "json_guard.h"
#include "vendor/rapidjson/writer.h"
namespace libai { namespace sdk_detail {
class durable_error_t : public std::runtime_error {
public: explicit durable_error_t(const std::string &s):std::runtime_error(s){}
};
// Wall time survives process boundaries; steady time bounds one live invocation.
inline std::int64_t execution_wall_ms(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
inline std::int64_t execution_timeout_max(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::duration::max()).count()/2;}
struct execution_time_t {
    bool present=false;
    std::int64_t started=0,last_seen=0,timeout_ms=0;
    std::string origin,reference;
    void validate() const {
        if(!present)return;
        if(started<=0||last_seen<started||timeout_ms<0||timeout_ms>execution_timeout_max()||started>INT64_MAX-timeout_ms||
            (origin!="initial_run"&&origin!="host_adoption")||(origin=="host_adoption"&&(reference.empty()||reference.size()>2048))||
            (origin=="initial_run"&&!reference.empty()))throw durable_error_t("invalid execution time budget");
    }
    void observe(){validate();if(!present)return;auto now=execution_wall_ms();if(now<last_seen||now<started)throw durable_error_t("execution wall clock moved backwards");last_seen=now;}
    void begin(std::int64_t start,std::int64_t timeout,const std::string &why="initial_run",const std::string &receipt="") {
        present=true;started=start;last_seen=start;timeout_ms=timeout;origin=why;reference=receipt;observe();
    }
    bool expired(){observe();return present&&timeout_ms&&last_seen>=started+timeout_ms;}
    void tighten(std::int64_t timeout){observe();if(timeout<0||timeout>execution_timeout_max())throw durable_error_t("invalid execution timeout");if(timeout&&(!timeout_ms||timeout<timeout_ms))timeout_ms=timeout;validate();}
    std::chrono::steady_clock::time_point deadline(){observe();if(!timeout_ms)return std::chrono::steady_clock::time_point::max();auto remaining=started+timeout_ms-last_seen;return std::chrono::steady_clock::now()+std::chrono::milliseconds(std::max<std::int64_t>(remaining,0));}
};
// One leased task checkpoint. Intent is committed before an application effect.
class durable_checkpoint_t {
public:
    struct effect_t {
        tool_call_t call;
        std::string key, schema, revision, state="ready", evidence;
        bool side_effect_free=false;
        std::size_t attempts=0;
    };
    execution_time_t time_budget;
    std::vector<effect_t> effects;
    std::size_t cursor=0;
    std::string phase="model";
    std::string output_schema, output_failure;
    std::size_t output_max_repairs=0, output_repair_count=0;
    static const char *feedback_prefix(){return "LIBAI_OUTPUT_VALIDATION_V1: ";}
private:
    std::shared_ptr<execution_store_t> store_;
    std::string scope_, revision_, input_, instructions_;
    bool opened_=false;
    std::size_t history_size_=0;
    using value_t=governance_detail::json::Value;
    static std::string text(const value_t &v,const char *key) {
        if (!v.IsObject() || !v.HasMember(key) || !v[key].IsString()) throw durable_error_t("invalid execution checkpoint string");
        return std::string(v[key].GetString(),v[key].GetStringLength());
    }
    static std::size_t number(const value_t &v,const char *key) {
        if (!v.IsObject() || !v.HasMember(key) || !v[key].IsUint64() || v[key].GetUint64()>std::numeric_limits<std::size_t>::max())
            throw durable_error_t("invalid execution checkpoint counter");
        return static_cast<std::size_t>(v[key].GetUint64());
    }
    static const value_t &array(const value_t &v,const char *key) {
        if (!v.IsObject() || !v.HasMember(key) || !v[key].IsArray()) throw durable_error_t("invalid execution checkpoint array");
        return v[key];
    }
    static tool_call_t read_call(const value_t &v) {
        tool_call_t call;call.id=text(v,"id");call.name=text(v,"name");call.arguments=text(v,"arguments");return call;
    }
    using writer_t=governance_detail::json::Writer<governance_detail::json::StringBuffer>;
    static void string(writer_t &w,const char *key,const std::string &s) {
        w.Key(key);w.String(s.data(),static_cast<governance_detail::json::SizeType>(s.size()));
    }
    static void count(writer_t &w,const char *key,std::size_t n){w.Key(key);w.Uint64(n);}
    static void write_call(writer_t &w,const tool_call_t &c) {
        string(w,"id",c.id);string(w,"name",c.name);string(w,"arguments",c.arguments);
    }
public:
    durable_checkpoint_t(std::shared_ptr<execution_store_t> store,const std::string &scope,
        const std::string &input,const std::string &instructions):store_(std::move(store)),scope_(scope),input_(input),instructions_(instructions) {
        if (scope.empty() || scope.size()>128) throw durable_error_t("execution scope must contain 1 to 128 ASCII identifier characters");
        for (unsigned char c:scope) if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'))
            throw durable_error_t("invalid execution scope");
    }
    ~durable_checkpoint_t(){if(opened_)store_->close(scope_);}
    bool load(agent_session_t &session,run_result_t &r,bool observe_time=true) {
        execution_snapshot_t snapshot;
        try{snapshot=store_->open(scope_);opened_=true;}catch(...){throw durable_error_t("execution store open/lease failed");}
        revision_=snapshot.revision;
        if (snapshot.state_json.empty()) {
            if (!revision_.empty()) throw durable_error_t("missing execution state at existing revision");
            history_size_=session.messages.size();
            return false;
        }
        if (revision_.empty()) throw durable_error_t("execution state has no revision");
        governance_detail::json::Document d;
        try{governance_detail::parse_json(snapshot.state_json,d);}catch(...){throw durable_error_t("corrupt execution checkpoint JSON");}
        if (number(d,"version")!=1 || text(d,"scope")!=scope_ || text(d,"input")!=input_ || text(d,"instructions")!=instructions_)
            throw durable_error_t("execution scope/task configuration mismatch");
        if(d.HasMember("time_budget")) {
            const auto &t=d["time_budget"];
            if(!t.IsObject()||t.MemberCount()!=6||number(t,"version")!=1)throw durable_error_t("invalid execution time budget object");
            auto started=number(t,"started_unix_ms"),last=number(t,"last_seen_unix_ms"),timeout=number(t,"timeout_ms");
            if(started>INT64_MAX||last>INT64_MAX||timeout>INT64_MAX)throw durable_error_t("execution time budget overflow");
            time_budget.present=true;time_budget.started=started;time_budget.last_seen=last;time_budget.timeout_ms=timeout;
            time_budget.origin=text(t,"origin");time_budget.reference=text(t,"reference");
            time_budget.validate();if(observe_time)time_budget.observe();
        }
        output_schema=d.HasMember("output_schema")?text(d,"output_schema"):"";
        output_failure=d.HasMember("output_failure")?text(d,"output_failure"):"";
        output_max_repairs=d.HasMember("output_max_repairs")?number(d,"output_max_repairs"):0;
        output_repair_count=d.HasMember("output_repair_count")?number(d,"output_repair_count"):0;
        if(output_max_repairs>8 || output_repair_count>output_max_repairs || output_failure.size()>2048 ||
            (output_schema.empty() && (output_max_repairs || output_repair_count || !output_failure.empty())))
            throw durable_error_t("invalid output contract counters");
        if(!output_schema.empty()){try{json_schema_t check(output_schema);}catch(...){throw durable_error_t("invalid output contract schema");}}
        phase=text(d,"phase");cursor=number(d,"cursor");history_size_=number(d,"history_size");
        r.model_calls=number(d,"model_calls");r.tool_calls=number(d,"tool_calls");
        r.input_tokens=number(d,"input_tokens");r.output_tokens=number(d,"output_tokens");r.text=text(d,"text");
        agent_session_t restored;
        for (const auto &v:array(d,"messages").GetArray()) {
            message_t m;auto role=text(v,"role");
            if(role=="user")m.role=message_t::role_t::user;
            else if(role=="assistant")m.role=message_t::role_t::assistant;
            else if(role=="tool")m.role=message_t::role_t::tool;
            else throw durable_error_t("invalid checkpoint message role");
            m.text=text(v,"text");m.call_id=text(v,"call_id");m.reasoning=text(v,"reasoning");
            if (!v.HasMember("tool_error") || !v["tool_error"].IsBool()) throw durable_error_t("invalid checkpoint tool result flag");
            m.tool_error=v["tool_error"].GetBool();
            for(const auto &c:array(v,"calls").GetArray())m.calls.push_back(read_call(c));
            restored.messages.push_back(m);
        }
        if(restored.messages.size()<=history_size_ || restored.messages[history_size_].role!=message_t::role_t::user || restored.messages[history_size_].text!=input_)
            throw durable_error_t("checkpoint task input message mismatch");
        std::set<std::string> ids;
        for(const auto &v:array(d,"effects").GetArray()) {
            effect_t e;e.call=read_call(v);e.key=text(v,"key");e.schema=text(v,"schema");
            e.revision=text(v,"revision");e.state=text(v,"state");e.evidence=text(v,"evidence");
            if(v.HasMember("side_effect_free")) {
                if(!v["side_effect_free"].IsBool())throw durable_error_t("invalid effect policy");
                e.side_effect_free=v["side_effect_free"].GetBool();
            }
            e.attempts=v.HasMember("attempts")?number(v,"attempts"):
                ((e.state=="running" || (e.state=="done"&&e.evidence!="not executed"))?1:0);
            if((e.state=="ready" && e.attempts) || (e.state=="running" && !e.attempts) ||
                (e.state=="done" && ((e.evidence=="not executed") != (e.attempts==0))) ||
                (!e.side_effect_free && e.attempts>1))
                throw durable_error_t("invalid effect attempt state");
            if(e.key!=scope_+"."+std::to_string(effects.size()) || e.call.id.empty() || !ids.insert(e.call.id).second ||
                (e.state!="ready"&&e.state!="running"&&e.state!="done"))throw durable_error_t("invalid execution effect identity/state");
            effects.push_back(e);
        }
        if(cursor>effects.size() || (phase!="tools"&&phase!="model"&&phase!="finished") ||
            (phase=="tools"?cursor==effects.size():cursor!=effects.size()))throw durable_error_t("invalid checkpoint execution cursor");
        for(std::size_t i=0;i<effects.size();++i) {
            if((i<cursor)!=(effects[i].state=="done") || (i>cursor&&effects[i].state!="ready"))
                throw durable_error_t("invalid checkpoint effect ordering");
            std::size_t matches=0;
            for(std::size_t m=history_size_;m<restored.messages.size();++m) for(const auto &c:restored.messages[m].calls)
                if(c.id==effects[i].call.id && c.name==effects[i].call.name && c.arguments==effects[i].call.arguments)++matches;
            if(matches!=1)throw durable_error_t("checkpoint effect lacks its original assistant call");
            std::size_t results=0;
            for(std::size_t m=history_size_;m<restored.messages.size();++m)
                if(restored.messages[m].role==message_t::role_t::tool && restored.messages[m].call_id==effects[i].call.id)++results;
            if(results!=(i<cursor?1u:0u))throw durable_error_t("checkpoint effect/result pairing mismatch");
        }
        std::size_t attempts=0,model_turns=0;
        for(const auto &e:effects) {
            if(e.attempts>std::numeric_limits<std::size_t>::max()-attempts)
                throw durable_error_t("effect attempt counter overflow");
            attempts+=e.attempts;
        }
        for(std::size_t i=history_size_;i<restored.messages.size();++i)
            if(restored.messages[i].role==message_t::role_t::assistant)++model_turns;
        if(r.tool_calls!=attempts || r.model_calls<model_turns)throw durable_error_t("checkpoint counters contradict committed execution");
        if(phase=="finished" && (restored.messages.back().role!=message_t::role_t::assistant ||
            !restored.messages.back().calls.empty() || restored.messages.back().text!=r.text))
            throw durable_error_t("checkpoint final result mismatch");
        std::size_t feedback_count=0;
        for(std::size_t i=history_size_+1;i<restored.messages.size();++i)
            if(restored.messages[i].role==message_t::role_t::user &&
                restored.messages[i].text.compare(0,std::string(feedback_prefix()).size(),feedback_prefix())==0)++feedback_count;
        if(feedback_count!=output_repair_count || (phase=="tools" && output_repair_count) ||
            (!output_failure.empty() && phase!="model"))throw durable_error_t("output feedback state mismatch");
        if(phase=="finished" && !output_schema.empty() && !json_schema_t(output_schema).validate(r.text).valid)
            throw durable_error_t("cached final output violates schema");
        session.messages.swap(restored.messages);
        return true;
    }
    void save(const agent_session_t &session,const run_result_t &r) {
        time_budget.observe();
        governance_detail::json::StringBuffer buffer;writer_t w(buffer);w.StartObject();
        if(time_budget.present){w.Key("time_budget");w.StartObject();count(w,"version",1);count(w,"started_unix_ms",time_budget.started);count(w,"last_seen_unix_ms",time_budget.last_seen);count(w,"timeout_ms",time_budget.timeout_ms);string(w,"origin",time_budget.origin);string(w,"reference",time_budget.reference);w.EndObject();}
        string(w,"output_schema",output_schema);string(w,"output_failure",output_failure);
        count(w,"output_max_repairs",output_max_repairs);count(w,"output_repair_count",output_repair_count);
        count(w,"version",1);string(w,"scope",scope_);string(w,"input",input_);string(w,"instructions",instructions_);
        string(w,"phase",phase);count(w,"cursor",cursor);count(w,"history_size",history_size_);string(w,"text",r.text);
        count(w,"model_calls",r.model_calls);count(w,"tool_calls",r.tool_calls);
        count(w,"input_tokens",r.input_tokens);count(w,"output_tokens",r.output_tokens);
        w.Key("messages");w.StartArray();
        for(const auto &m:session.messages) {
            w.StartObject();string(w,"role",m.role==message_t::role_t::user?"user":m.role==message_t::role_t::assistant?"assistant":"tool");
            string(w,"text",m.text);string(w,"call_id",m.call_id);string(w,"reasoning",m.reasoning);
            w.Key("tool_error");w.Bool(m.tool_error);w.Key("calls");w.StartArray();
            for(const auto &c:m.calls){w.StartObject();write_call(w,c);w.EndObject();}w.EndArray();w.EndObject();
        }w.EndArray();w.Key("effects");w.StartArray();
        for(const auto &e:effects) {
            w.StartObject();write_call(w,e.call);string(w,"key",e.key);string(w,"schema",e.schema);
            string(w,"revision",e.revision);string(w,"state",e.state);string(w,"evidence",e.evidence);
            w.Key("side_effect_free");w.Bool(e.side_effect_free);w.Key("attempts");w.Uint64(e.attempts);w.EndObject();
        }w.EndArray();w.EndObject();
        std::string wire(buffer.GetString(),buffer.GetSize());
        governance_detail::json::Document check;
        try{governance_detail::parse_json(wire,check);}catch(...){throw durable_error_t("execution checkpoint exceeds JSON limits or contains invalid UTF-8");}
        try {
            auto next=store_->commit(scope_,revision_,wire);
            if(next.empty() || next==revision_)throw durable_error_t("execution store did not return a new revision");
            revision_=next;
        }catch(...){throw durable_error_t("execution checkpoint commit failed; effect outcome may require reconciliation");}
    }
    void prepare(const std::vector<tool_call_t> &calls,const std::map<std::string,tool_t> &tools) {
        phase="tools";
        for(const auto &c:calls) {
            effect_t e;e.call=c;e.key=scope_+"."+std::to_string(effects.size());auto found=tools.find(c.name);
            if(found!=tools.end()){e.schema=found->second.spec.parameters;e.revision=found->second.spec.execution_revision;e.side_effect_free=found->second.spec.side_effect_free;}
            effects.push_back(e);
        }
    }
    model_response_t pending(const std::map<std::string,tool_t> &tools) const {
        model_response_t response;
        for(std::size_t i=cursor;i<effects.size();++i) {
            const auto &e=effects[i];
            auto found=tools.find(e.call.name);
            const auto schema=found==tools.end()?"":found->second.spec.parameters;
            const auto revision=found==tools.end()?"":found->second.spec.execution_revision;
            const bool side_effect_free=found!=tools.end() && found->second.spec.side_effect_free;
            if(schema!=e.schema || revision!=e.revision || side_effect_free!=e.side_effect_free)throw durable_error_t("pending tool definition changed; reconciliation required");
            if(e.state=="running" && !e.side_effect_free)throw durable_error_t("unconfirmed tool effect requires host reconciliation: "+e.key);
            response.calls.push_back(e.call);
        }
        return response;
    }
    const std::string &revision() const{return revision_;}
    const std::string &key() const{return effects.at(cursor).key;}
    bool running() const{return cursor<effects.size()&&effects[cursor].state=="running";}
    bool side_effect_free() const{return effects.at(cursor).side_effect_free;}
    void start(){
        auto &effect=effects.at(cursor);
        if(effect.attempts==std::numeric_limits<std::size_t>::max())throw durable_error_t("effect attempts exhausted");
        effect.state="running";++effect.attempts;
    }
    void done(const std::string &evidence="tool returned success"){
        effects.at(cursor).state="done";effects.at(cursor).evidence=evidence;++cursor;
        if(cursor==effects.size())phase="model";
    }
};
}}

namespace libai {
// Trusted host reconciliation only: verify the external receipt before calling.
// This is deliberately not registered as an agent tool.
inline void confirm_execution_effect(std::shared_ptr<execution_store_t> store,
    const std::string &scope,const std::string &input,const std::string &instructions,
    const std::string &expected_revision,const std::string &key,const tool_result_t &output,
    const std::string &outcome,const std::string &reference) {
    if(!store || expected_revision.empty() || reference.empty() || reference.size()>2048 ||
        (outcome!="none"&&outcome!="applied") || (outcome=="applied"&&output.error))
        throw std::invalid_argument("reconciliation needs a receipt, revision and definite effect outcome");
    sdk_detail::durable_checkpoint_t checkpoint(store,scope,input,instructions);
    agent_session_t session;run_result_t result;
    if(!checkpoint.load(session,result) || checkpoint.phase!="tools" || !checkpoint.running() ||
        checkpoint.revision()!=expected_revision || checkpoint.key()!=key)
        throw std::invalid_argument("stale or mismatched execution reconciliation");
    message_t message;message.role=message_t::role_t::tool;message.call_id=checkpoint.effects.at(checkpoint.cursor).call.id;
    message.text=output.text;message.tool_error=output.error;session.messages.push_back(message);
    checkpoint.done(outcome+":"+reference);checkpoint.save(session,result);
}
}

namespace libai {
// Trusted legacy migration. The host independently determines the remaining budget;
// this does not recover unknown historical time or authorize/reconcile effects.
inline void adopt_execution_deadline(std::shared_ptr<execution_store_t> store,const std::string &scope,
    const std::string &input,const std::string &instructions,const std::string &revision,
    std::chrono::milliseconds remaining,const std::string &reference) {
    if(!store||revision.empty()||reference.empty()||reference.size()>2048||remaining.count()<0||remaining.count()>sdk_detail::execution_timeout_max())throw std::invalid_argument("legacy deadline adoption requires verified budget/revision/reference");
    sdk_detail::durable_checkpoint_t checkpoint(store,scope,input,instructions);agent_session_t session;run_result_t result;
    if(!checkpoint.load(session,result)||checkpoint.revision()!=revision||checkpoint.phase=="finished"||checkpoint.time_budget.present)throw std::invalid_argument("stale or nonlegacy deadline adoption");
    checkpoint.time_budget.begin(sdk_detail::execution_wall_ms(),remaining.count(),"host_adoption",reference);checkpoint.save(session,result);
}
}
