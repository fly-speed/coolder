#pragma once
#include "agent_types.h"
#include "vendor/rapidjson/writer.h"
#include <cstdint>
namespace libai { namespace workflow_storage {
namespace json=governance_detail::json;
inline std::string wire(const json::Value &v){json::StringBuffer b;json::Writer<json::StringBuffer>w(b);v.Accept(w);return {b.GetString(),b.GetSize()};}
inline void field(json::Document &d,const char *key,const std::string &s){json::Value v;v.SetString(s.data(),static_cast<json::SizeType>(s.size()),d.GetAllocator());d.AddMember(json::StringRef(key),v,d.GetAllocator());}
inline std::string text(const json::Value &d,const char *key){if(!d.IsObject()||!d.HasMember(key)||!d[key].IsString())throw std::runtime_error("workflow journal string invalid");return {d[key].GetString(),d[key].GetStringLength()};}
inline std::uint64_t number(const json::Value &d,const char *key){if(!d.HasMember(key)||!d[key].IsUint64())throw std::runtime_error("workflow journal number invalid");return d[key].GetUint64();}
inline std::int64_t now(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
struct entry {std::string phase="pending",patch="{}",route,receipt;std::size_t attempts=0;};
class journal {
    std::shared_ptr<execution_store_t> store_;std::string key_;bool leased_=false;
public:
    std::string revision,definition,initial,state,wave_input,error;
    std::map<std::string,entry> entries;std::vector<std::string>wave;
    std::size_t calls=0,budget=64,state_limit=2*1024*1024;
    std::int64_t started=0,last_seen=0,timeout_ms=0;
    bool loaded=false,finished=false;run_status_t status=run_status_t::completed;
    journal(std::shared_ptr<execution_store_t> store,const std::string &scope):store_(std::move(store)){
        if(!store_||scope.empty())throw std::invalid_argument("durable workflow needs store and stable scope");
        key_="wfg"+std::to_string(scope.size())+"_"+scope;
        auto snapshot=store_->open(key_);leased_=true;revision=snapshot.revision;
        try{
            if(snapshot.state_json.empty()){if(!revision.empty())throw std::runtime_error("workflow journal revision without state");return;}
            if(revision.empty())throw std::runtime_error("workflow journal state without revision");
            json::Document d;governance_detail::parse_json_bounded(snapshot.state_json,d,16*1024*1024);
            if(!d.IsObject()||number(d,"version")!=1||text(d,"kind")!="libai.workflow")throw std::runtime_error("workflow journal version invalid");
            definition=text(d,"definition");initial=text(d,"initial");state=text(d,"state");wave_input=text(d,"wave_input");error=text(d,"error");
            calls=number(d,"calls");budget=number(d,"budget");state_limit=number(d,"state_limit");
            auto start=number(d,"started"),last=number(d,"last_seen"),timeout=number(d,"timeout_ms"),code=number(d,"status");
            if(calls>64||budget>64||state_limit<1||state_limit>2*1024*1024||start==0||start>INT64_MAX||last<start||last>INT64_MAX||timeout>86400000||code>static_cast<unsigned>(run_status_t::recovery_required))throw std::runtime_error("workflow journal bounds invalid");
            started=start;last_seen=last;timeout_ms=timeout;status=static_cast<run_status_t>(code);
            if(!d.HasMember("finished")||!d["finished"].IsBool()||!d.HasMember("nodes")||!d["nodes"].IsObject()||!d.HasMember("wave")||!d["wave"].IsArray())throw std::runtime_error("workflow journal containers invalid");
            finished=d["finished"].GetBool();if(d["nodes"].MemberCount()>64||d["wave"].Size()>64)throw std::runtime_error("workflow journal size invalid");
            for(auto i=d["nodes"].MemberBegin();i!=d["nodes"].MemberEnd();++i){entry e;e.phase=text(i->value,"phase");e.patch=text(i->value,"patch");e.route=text(i->value,"route");e.receipt=text(i->value,"receipt");e.attempts=number(i->value,"attempts");if(e.attempts>64)throw std::runtime_error("workflow node attempts invalid");entries.emplace(std::string(i->name.GetString(),i->name.GetStringLength()),std::move(e));}
            for(const auto &v:d["wave"].GetArray()){if(!v.IsString())throw std::runtime_error("workflow wave identity invalid");wave.emplace_back(v.GetString(),v.GetStringLength());}
            loaded=true;
        }catch(...){store_->close(key_);leased_=false;throw;}
    }
    ~journal(){if(leased_)store_->close(key_);}
    journal(const journal&)=delete;journal&operator=(const journal&)=delete;
    void save(){
        auto time=now();if(time<last_seen||time<started)throw std::runtime_error("workflow wall clock moved backwards");last_seen=time;
        json::Document d;d.SetObject();d.AddMember("version",1,d.GetAllocator());field(d,"kind","libai.workflow");field(d,"definition",definition);field(d,"initial",initial);field(d,"state",state);field(d,"wave_input",wave_input);field(d,"error",error);
        d.AddMember("calls",static_cast<std::uint64_t>(calls),d.GetAllocator());d.AddMember("budget",static_cast<std::uint64_t>(budget),d.GetAllocator());d.AddMember("state_limit",static_cast<std::uint64_t>(state_limit),d.GetAllocator());d.AddMember("started",started,d.GetAllocator());d.AddMember("last_seen",last_seen,d.GetAllocator());d.AddMember("timeout_ms",timeout_ms,d.GetAllocator());d.AddMember("status",static_cast<unsigned>(status),d.GetAllocator());d.AddMember("finished",finished,d.GetAllocator());
        json::Value nodes(json::kObjectType),batch(json::kArrayType);
        for(const auto &i:entries){json::Document e;e.SetObject();field(e,"phase",i.second.phase);field(e,"patch",i.second.patch);field(e,"route",i.second.route);field(e,"receipt",i.second.receipt);e.AddMember("attempts",static_cast<std::uint64_t>(i.second.attempts),e.GetAllocator());json::Value k,v;k.SetString(i.first.data(),static_cast<json::SizeType>(i.first.size()),d.GetAllocator());v.CopyFrom(e,d.GetAllocator());nodes.AddMember(k,v,d.GetAllocator());}
        for(const auto &id:wave){json::Value v;v.SetString(id.data(),static_cast<json::SizeType>(id.size()),d.GetAllocator());batch.PushBack(v,d.GetAllocator());}d.AddMember("nodes",nodes,d.GetAllocator());d.AddMember("wave",batch,d.GetAllocator());
        auto serialized=wire(d);json::Document checked;governance_detail::parse_json_bounded(serialized,checked,16*1024*1024);
        auto next=store_->commit(key_,revision,serialized);if(next.empty()||next==revision)throw std::runtime_error("workflow journal revision did not advance");revision=std::move(next);
    }
};
}}
