#pragma once
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace libai { namespace ai {

// Bounded lexical review signals, NOT a type checker or coverage certificate.
// Sources are data; this never executes project code. Findings deliberately do
// not fail validation: adapters/custom serializers may make a candidate valid.
struct source_contract_finding_t {
    std::string kind, path, related_path, detail;
    size_t line = 0, related_line = 0;
};
namespace source_contract_detail {
struct token_t { std::string text; size_t line; bool literal; };
inline bool identifier(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}
inline bool ends(const std::string &value, const std::string &suffix) {
    return value.size() >= suffix.size() && value.compare(value.size()-suffix.size(),suffix.size(),suffix)==0;
}
inline std::vector<token_t> tokens(const std::string &text, size_t first_line = 1) {
    std::vector<token_t> out;
    size_t line = first_line;
    for (size_t i=0; i<text.size();) {
        const char c=text[i];
        if (std::isspace(static_cast<unsigned char>(c))) { if(c=='\n')++line; ++i; continue; }
        if (i+1<text.size() && c=='/' && text[i+1]=='/') {
            while(i<text.size() && text[i]!='\n')++i;
            continue;
        }
        if (i+1<text.size() && c=='/' && text[i+1]=='*') {
            i+=2;
            while(i<text.size() && !(text[i]=='*' && i+1<text.size() && text[i+1]=='/')) {
                if(text[i++]=='\n')++line;
            }
            i=std::min(i+2,text.size());continue;
        }
        const auto start_line=line;
        if(c=='"' || c=='\'' || c=='`') {
            std::string value; ++i;
            while(i<text.size() && text[i]!=c) {
                if(text[i]=='\n')++line;
                if(c!='`' && text[i]=='\\' && i+1<text.size()) {
                    // Preserve escaped characters without treating them as code.
                    value+=text[i++]; value+=text[i++];
                } else value+=text[i++];
            }
            if(i<text.size())++i;
            out.push_back({value,start_line,true});continue;
        }
        const size_t begin=i++;
        if(identifier(c))while(i<text.size() && identifier(text[i]))++i;
        out.push_back({text.substr(begin,i-begin),start_line,false});
    }
    return out;
}
inline std::map<std::string,size_t> js_properties(const std::string &source, size_t line=1) {
    const auto ts=tokens(source,line);std::map<std::string,size_t> result;
    for(size_t i=1;i+1<ts.size();++i) {
        if(ts[i].literal || ts[i].text!="." || ts[i+1].literal || ts[i-1].literal)continue;
        const auto &name=ts[i+1].text;
        if(!name.empty() && std::islower(static_cast<unsigned char>(name[0])))result.emplace(name,ts[i+1].line);
    }
    return result;
}
}

inline std::vector<source_contract_finding_t> source_contract_review(
    const std::map<std::string,std::string> &sources)
{
    using namespace source_contract_detail;
    struct field_t { std::string name,path,type; size_t line; };
    std::vector<field_t> fields;
    std::map<std::string,std::pair<std::string,size_t>> consumers;
    std::vector<source_contract_finding_t> findings;
    bool json_used=false, custom_json=false;
    std::string handshake_path, frame_path;size_t transport_line=1;
    std::string timer_path, setter_path;size_t timer_line=1, setter_line=1;bool timer_reset=false;
    size_t bytes=0,files=0;
    for(const auto &file:sources) {
        if(++files>128 || (bytes+=file.second.size())>1024*1024)break;
        if(file.second.size()>256*1024)continue;
        const auto ts=tokens(file.second);
        const bool go_source=ends(file.first,".go"), test=ends(file.first,"_test.go");
        auto collect_js=[&](const std::string &text,size_t line) {
            for(const auto &prop:js_properties(text,line))consumers.emplace(prop.first,std::make_pair(file.first,prop.second));
        };
        if(ends(file.first,".js") || ends(file.first,".ts") || ends(file.first,".html"))collect_js(file.second,1);
        if(!go_source)continue;
        bool handshake=false, frame_reader=false;size_t handshake_line=1;
        std::map<std::string,std::vector<size_t>> discarded;
        std::set<std::string> local_functions;
        for(size_t i=0;i<ts.size();++i) {
            const auto &t=ts[i];
            if(t.literal) {
                if(!test && t.text=="encoding/json")json_used=true;
                if(!test && (t.text=="Sec-WebSocket-Accept" || t.text.find("Sec-WebSocket-Accept:")!=std::string::npos)) {
                    handshake=true;handshake_line=t.line;
                }
                if(!test && t.text.find("<script")!=std::string::npos)collect_js(t.text,t.line);
                continue;
            }
            if(!test && t.text=="MarshalJSON")custom_json=true;
            if(!test && t.text=="NewTicker") {timer_path=file.first;timer_line=t.line;}
            if(!test && (t.text=="Reset" || t.text=="After" || t.text=="NewTimer"))timer_reset=true;
            if(!test && (t.text=="SetSpeed" || t.text=="SetRate" || t.text=="SetInterval" || t.text=="SetPeriod")) {
                setter_path=file.first;setter_line=t.line;
            }
            if(!test && (t.text=="ReadByte" || t.text=="ReadFull"))frame_reader=true;
            if(test && i+3<ts.size() && ts[i+1].text==":" && ts[i+2].text=="=" && ts[i+3].text=="func")
                local_functions.insert(t.text);
            if(test && t.text=="_" && i+3<ts.size() && ts[i+1].text=="=" && !ts[i+2].literal && ts[i+3].text=="(")
                discarded[ts[i+2].text].push_back(t.line);
            if(test || t.text!="type" || i+3>=ts.size() || ts[i+2].text!="struct" || ts[i+3].text!="{")continue;
            const std::string type=ts[i+1].text;
            // Inspect simple named fields only. Embedded/anonymous fields and
            // semicolon-packed declarations stay outside this lexical check.
            for(size_t j=i+4;j<ts.size() && ts[j].text!="}";) {
                size_t end=j+1;
                while(end<ts.size() && ts[end].line==ts[j].line && ts[end].text!="}")++end;
                bool tag=false,complex=false;
                for(size_t k=j;k<end;++k) {
                    tag=tag || (ts[k].literal && ts[k].text.find("json:")!=std::string::npos);
                    complex=complex || ts[k].text=="{" || ts[k].text==";";
                }
                size_t field_type=j+1;
                while(field_type+1<end && ts[field_type].text==",")field_type+=2;
                static const std::set<std::string> scalars={"int","int8","int16","int32","int64","uint","uint8","uint16","uint32","uint64","float32","float64","bool","string"};
                if(!tag && !complex && field_type<end && scalars.count(ts[field_type].text) && !ts[j].literal && !ts[j].text.empty() &&
                    std::isupper(static_cast<unsigned char>(ts[j].text[0]))) {
                    fields.push_back({ts[j].text,file.first,type,ts[j].line});
                    for(size_t k=j+1;k+2<end && ts[k].text==",";k+=2)
                        if(!ts[k+1].text.empty() && std::isupper(static_cast<unsigned char>(ts[k+1].text[0])))
                            fields.push_back({ts[k+1].text,file.first,type,ts[k+1].line});
                }
                j=end;
            }
        }
        if(handshake) {handshake_path=file.first;transport_line=handshake_line;}
        if(frame_reader)frame_path=file.first;
        for(const auto &call:discarded)if(call.second.size()>1 && local_functions.count(call.first) && findings.size()<12)
            findings.push_back({"discarded_test_observations",file.first,"",
                "Repeated results of local test helper '"+call.first+"' are discarded. Check whether the requested comparison is actually asserted; helper-local assertions may cover a different property.",call.second[0],0});
    }
    if(!handshake_path.empty() && !frame_path.empty() && findings.size()<12)
        findings.push_back({"manual_websocket_transport",handshake_path,frame_path,
            "Hand-written WebSocket handshake and frame reader, possibly in different files. Exercise actual masked client messages, extended lengths and malformed input; HTTP readiness is not transport evidence.",transport_line,0});
    if(!timer_path.empty() && !setter_path.empty() && !timer_reset && findings.size()<12)
        findings.push_back({"runtime_timer_configuration",timer_path,setter_path,
            "A ticker is created and runtime timing configuration is exposed, but no timer reset was observed. Check whether the live scheduler applies configuration after startup. Compare actual updates over equal elapsed time; returning the new setting alone is not evidence. This lexical signal may be inapplicable to a recreated or custom scheduler.",timer_line,setter_line});
    if(json_used && !custom_json)for(const auto &field:fields) {
        std::string lower=field.name;lower[0]=static_cast<char>(std::tolower(static_cast<unsigned char>(lower[0])));
        auto use=consumers.find(lower);
        if(use==consumers.end() || findings.size()>=12)continue;
        findings.push_back({"json_field_case_boundary",field.path,use->second.first,
            "Go field "+field.type+"."+field.name+" has no JSON tag; JavaScript reads ."+lower+
            ". Confirm these are the same payload using the actual serialized response and consumer keys. A typed Go round trip alone cannot establish browser compatibility.",field.line,use->second.second});
    }
    return findings;
}
} }
