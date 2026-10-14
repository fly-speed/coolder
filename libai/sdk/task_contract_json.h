#pragma once
#include "task_contract.h"
#include "json_guard.h"
#include "vendor/rapidjson/writer.h"
#include <set>

namespace libai {
namespace contract_detail {
using value_t=governance_detail::json::Value;
inline std::string text(const value_t &object,const char *key){
    if(!object.HasMember(key))return "";
    const auto &value=object[key];
    if(!value.IsString())throw std::invalid_argument(std::string(key)+" must be a string");
    std::string result(value.GetString(),value.GetStringLength());
    if(result.find('\0')!=std::string::npos)throw std::invalid_argument("task evidence contains NUL");
    return result;
}
inline bool boolean(const value_t &object,const char *key,bool fallback){
    if(!object.HasMember(key))return fallback;
    if(!object[key].IsBool())throw std::invalid_argument(std::string(key)+" must be boolean");return object[key].GetBool();
}
inline task_contract_t parse(const value_t &args){
    if(!args.IsObject())throw std::invalid_argument("task contract must be an object");
    if(args.HasMember("version")&&(!args["version"].IsUint()||args["version"].GetUint()!=1))
        throw std::invalid_argument("unsupported task contract version");
    task_contract_t contract;
    contract.loop_finished=boolean(args,"loop_finished",false);
    contract.loop_outcome=text(args,"loop_outcome");
    static const std::set<std::string> outcomes={"","running","completed","cancelled","deadline_exceeded",
        "budget_exceeded","model_error","protocol_error","callback_error","recovery_required"};
    if(!outcomes.count(contract.loop_outcome))throw std::invalid_argument("invalid loop outcome");
    contract.artifact_revision=text(args,"artifact_revision");
    if(args.HasMember("requirements")){
        const auto &requirements=args["requirements"];
        if(!requirements.IsObject())throw std::invalid_argument("requirements must be an object");
        contract.require_generation=boolean(requirements,"generation",true);
        contract.require_build=boolean(requirements,"build",true);
        contract.require_tests=boolean(requirements,"tests",true);
        contract.require_review=boolean(requirements,"review",true);
    }
    task_evidence_t *stages[]={&contract.generation,&contract.build,&contract.tests,&contract.review};
    const char *names[]={"generation","build","tests","review"};
    for(unsigned i=0;i<4;++i){
        if(!args.HasMember(names[i]))continue;
        const auto &object=args[names[i]];
        if(!object.IsObject())throw std::invalid_argument("evidence must be an object");
        auto &e=*stages[i];e.state=text(object,"state");e.revision=text(object,"revision");
        e.authority=text(object,"authority");e.reference=text(object,"reference");
        const std::string positive=i==0?"completed":i==3?"accepted":"passed";
        if(e.state!="unknown"&&e.state!=positive&&e.state!="failed"&&e.state!="not_required"&&!(i==3&&e.state=="rejected"))
            throw std::invalid_argument("invalid evidence state");
        if(!e.authority.empty()&&e.authority!="host"&&e.authority!="human"&&e.authority!="model")
            throw std::invalid_argument("invalid evidence authority");
    }
    return contract;
}
}
// Host evidence only: does not run tools, check receipt files, or approve work.
inline task_contract_t parse_task_contract_json(const std::string &arguments){
    governance_detail::json::Document args;governance_detail::parse_json(arguments,args);
    return contract_detail::parse(args);
}
inline std::string evaluate_task_contract_json(const std::string &arguments){
    namespace json=governance_detail::json;
    json::StringBuffer buffer;json::Writer<json::StringBuffer> out(buffer);out.StartObject();
    try{
        auto contract=parse_task_contract_json(arguments);
        const bool checks[]={contract.loop_finished&&contract.loop_outcome=="completed",
            !contract.require_generation||contract.proves(contract.generation,"completed","host"),
            !contract.require_build||contract.proves(contract.build,"passed","host"),
            !contract.require_tests||contract.proves(contract.tests,"passed","host"),
            !contract.require_review||contract.proves(contract.review,"accepted","human")};
        const char *names[]={"loop","generation","build","tests","review"};
        out.Key("ok");out.Bool(true);out.Key("version");out.Uint(1);out.Key("accepted");out.Bool(contract.accepted());
        out.Key("checks");out.StartObject();for(unsigned i=0;i<5;++i){out.Key(names[i]);out.Bool(checks[i]);}out.EndObject();
        out.Key("unmet");out.StartArray();for(unsigned i=0;i<5;++i)if(!checks[i])out.String(names[i]);out.EndArray();
    }catch(const std::exception &e){out.Key("ok");out.Bool(false);out.Key("accepted");out.Bool(false);out.Key("error");out.String(e.what());}
    out.EndObject();return std::string(buffer.GetString(),buffer.GetSize());
}
}
