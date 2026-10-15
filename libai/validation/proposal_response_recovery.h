#pragma once
#include "../sdk/json_schema.h"
#include "../sdk/vendor/rapidjson/error/en.h"
#include <string>

namespace libai { namespace ai {
inline std::string proposal_json_error(const std::string &wire) {
    namespace j = governance_detail::json;
    if (wire.size()>2*1024*1024 || wire.find('\0')!=std::string::npos) return "JSON byte limit or raw NUL";
    j::Reader reader; j::StringStream stream(wire.c_str()); governance_detail::json_guard guard;
    constexpr unsigned flags=j::kParseIterativeFlag|j::kParseValidateEncodingFlag|j::kParseFullPrecisionFlag;
    if (reader.Parse<flags>(stream,guard)) return "";
    return (guard.error.empty()?std::string(j::GetParseError_En(reader.GetParseErrorCode())):guard.error)+
        " at byte "+std::to_string(reader.GetErrorOffset());
}
// All repairs are explicit model edits, scoped to one retained response. No
// automatic duplicate-key/escape repair and no execution of another tool type.
inline bool recover_proposal_response(const std::string &saved, const std::string &reference,
    std::string &payload, std::string &error) {
    namespace j = governance_detail::json;
    try {
        j::Document ref; governance_detail::parse_json(reference,ref);
        if (!ref.IsObject()) throw std::invalid_argument("recovery reference must be an object");
        if (ref.HasMember("decode_tool") && !ref["decode_tool"].IsBool()) throw std::invalid_argument("decode_tool requires a boolean");
        payload=saved;
        if(ref.HasMember("raw_edits")) {
            const auto &edits=ref["raw_edits"];
            if(!edits.IsArray() || edits.Size()>16) throw std::invalid_argument("raw_edits requires at most 16 edits");
            for(const auto &edit:edits.GetArray()) {
                if(!edit.IsObject() || !edit.HasMember("old_text") || !edit["old_text"].IsString() ||
                    !edit.HasMember("new_text") || !edit["new_text"].IsString()) throw std::invalid_argument("raw edit requires string old_text/new_text");
                const std::string before(edit["old_text"].GetString(),edit["old_text"].GetStringLength());
                const std::string after(edit["new_text"].GetString(),edit["new_text"].GetStringLength());
                auto pos=payload.find(before);
                if(before.empty() || pos==std::string::npos || payload.find(before,pos+1)!=std::string::npos)
                    throw std::invalid_argument("raw edit old_text must match exactly once");
                payload.replace(pos,before.size(),after);
                if(payload.size()>2*1024*1024) throw std::invalid_argument("repaired response exceeds byte limit");
            }
        }
        if(ref.HasMember("decode_tool") && ref["decode_tool"].IsBool() && ref["decode_tool"].GetBool()) {
            // The stored fallback may be fenced; retain exact original bytes in
            // the archive but strip only an unambiguous outer JSON fence here.
            const auto first=payload.find_first_not_of(" \t\r\n"),last=payload.find_last_not_of(" \t\r\n");
            if(first!=std::string::npos && payload.compare(first,3,"```")==0 && last>=2 && payload.compare(last-2,3,"```")==0) {
                const auto line=payload.find('\n',first);
                if(line!=std::string::npos && line<last-2) payload=payload.substr(line+1,last-2-line-1);
            }
            const auto diagnostic=proposal_json_error(payload);
            if(!diagnostic.empty()) throw std::invalid_argument(diagnostic);
            static const std::string schema=R"({"type":"object","required":["type","name","arguments"],"additionalProperties":false,"properties":{"type":{"enum":["tool_call"]},"name":{"enum":["workspace.propose_batch"]},"arguments":{"type":"object","required":["content"],"additionalProperties":false,"properties":{"content":{"type":"string"}}}}})";
            const auto result=validate_json_schema(schema,payload);
            if(!result.valid) throw std::invalid_argument("recovered tool rejected: "+result.code+" "+result.instance_pointer);
            j::Document tool;governance_detail::parse_json(payload,tool);
            const auto &content=tool["arguments"]["content"];
            payload.assign(content.GetString(),content.GetStringLength());
        }
        const auto diagnostic=proposal_json_error(payload);
        if(!diagnostic.empty()) throw std::invalid_argument(diagnostic);
        j::Document batch;governance_detail::parse_json(payload,batch);
        if(!batch.IsArray()) throw std::invalid_argument("repaired batch must be a file array");
        return true;
    } catch(const std::exception &e) {error=e.what();payload.clear();return false;}
}
} }
