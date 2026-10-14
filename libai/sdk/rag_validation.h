#pragma once
#include "rag_types.h"
#include "json_guard.h"
#include "vendor/rapidjson/writer.h"
#include <cmath>
#include <set>
namespace libai { namespace rag_detail {
namespace json=governance_detail::json;
inline void stopped(const retrieval_context_t &c){if(c.stopped())throw std::runtime_error("retrieval cancelled or deadline exceeded");}
inline void utf8(const std::string &s,std::size_t maximum,bool empty=true){
    if(s.size()>maximum||(!empty&&s.empty()))throw std::invalid_argument("RAG text empty or exceeds limit");
    for(std::size_t i=0;i<s.size();){unsigned char b=s[i++];if(!b)throw std::invalid_argument("RAG text contains NUL");if(b<0x80)continue;
        unsigned count;unsigned char low=0x80,high=0xbf;
        if(b>=0xc2&&b<=0xdf)count=1;
        else if(b>=0xe0&&b<=0xef){count=2;if(b==0xe0)low=0xa0;if(b==0xed)high=0x9f;}
        else if(b>=0xf0&&b<=0xf4){count=3;if(b==0xf0)low=0x90;if(b==0xf4)high=0x8f;}
        else throw std::invalid_argument("RAG invalid UTF-8");
        if(s.size()-i<count||static_cast<unsigned char>(s[i])<low||static_cast<unsigned char>(s[i])>high)throw std::invalid_argument("RAG invalid UTF-8");
        for(unsigned n=0;n<count;++n){unsigned char c=s[i++];if(c<0x80||c>0xbf)throw std::invalid_argument("RAG invalid UTF-8");}
    }
}
inline void scope(const retrieval_context_t &c){utf8(c.access_scope,128,false);for(char b:c.access_scope)if(!((b>='a'&&b<='z')||(b>='A'&&b<='Z')||(b>='0'&&b<='9')||b=='_'||b=='-'))throw std::invalid_argument("RAG scope must be an explicit ASCII namespace");stopped(c);}
inline void limits(const rag_limits_t &l){
    if(!l.max_query_bytes||l.max_query_bytes>8192||!l.max_documents||l.max_documents>64||!l.max_chunks||l.max_chunks>256||!l.max_hits||l.max_hits>64||!l.max_document_bytes||l.max_document_bytes>2*1024*1024||!l.max_chunk_bytes||l.max_chunk_bytes>65536||!l.max_metadata_bytes||l.max_metadata_bytes>16384||!l.max_total_bytes||l.max_total_bytes>4*1024*1024||!l.max_dimension||l.max_dimension>16384)throw std::invalid_argument("RAG invalid limits");
}
inline json::Document object(const std::string &s,std::size_t maximum){json::Document d;governance_detail::parse_json_bounded(s,d,maximum);if(!d.IsObject())throw std::invalid_argument("RAG metadata/filter must be an object");return d;}
inline std::string wire(const json::Value &v){json::StringBuffer b;json::Writer<json::StringBuffer>w(b);v.Accept(w);return {b.GetString(),b.GetSize()};}
inline void field(json::Value &v,const char *key,const std::string &s,json::Document::AllocatorType &a){json::Value x;x.SetString(s.data(),static_cast<json::SizeType>(s.size()),a);v.AddMember(json::Value(key,a),x,a);}
inline void request(const retrieval_request_t &r,const retrieval_context_t &c,const rag_limits_t &l){limits(l);scope(c);utf8(r.query,l.max_query_bytes,false);if(!r.top_k||r.top_k>l.max_hits)throw std::invalid_argument("RAG top_k invalid");object(r.filter_json,l.max_metadata_bytes);}
inline std::size_t document(const document_t &d,const rag_limits_t &l){utf8(d.id,128,false);utf8(d.source,2048,false);utf8(d.revision,128,false);utf8(d.text,l.max_document_bytes);object(d.metadata_json,l.max_metadata_bytes);return d.id.size()+d.source.size()+d.revision.size()+d.text.size()+d.metadata_json.size();}
inline std::size_t chunk(const document_chunk_t &d,const rag_limits_t &l){utf8(d.id,512,false);utf8(d.document_id,128,false);utf8(d.source,2048,false);utf8(d.revision,128,false);utf8(d.text,l.max_chunk_bytes,false);object(d.metadata_json,l.max_metadata_bytes);
    if(d.has_span&&(d.end<d.begin||d.end-d.begin!=d.text.size()||d.end>2*1024*1024))throw std::invalid_argument("RAG byte span invalid");
    return d.id.size()+d.document_id.size()+d.source.size()+d.revision.size()+d.text.size()+d.metadata_json.size();}
inline bool matches(const std::string &metadata,const std::string &filter,std::size_t maximum){auto m=object(metadata,maximum),f=object(filter,maximum);for(auto i=f.MemberBegin();i!=f.MemberEnd();++i){auto found=m.FindMember(i->name);if(found==m.MemberEnd()||found->value!=i->value)return false;}return true;}
inline void vector(const std::vector<double> &v,const rag_limits_t &l){if(v.empty()||v.size()>l.max_dimension)throw std::invalid_argument("RAG vector dimension invalid");bool nonzero=false;for(double d:v){if(!std::isfinite(d))throw std::invalid_argument("RAG vector must be finite");nonzero=nonzero||d!=0;}if(!nonzero)throw std::invalid_argument("RAG zero vector");}
inline void hits(const retrieval_result_t &result,const retrieval_request_t &r,const retrieval_context_t &c,const rag_limits_t &l){
    if(result.hits.size()>r.top_k||result.hits.size()>l.max_hits)throw std::invalid_argument("RAG backend returned too many hits");
    std::size_t total=0;std::set<std::string>ids;
    for(const auto &hit:result.hits){total+=chunk(hit.chunk,l);if(total>l.max_total_bytes||!ids.insert(hit.chunk.id).second||hit.access_scope!=c.access_scope||!std::isfinite(hit.score)||!matches(hit.chunk.metadata_json,r.filter_json,l.max_metadata_bytes))throw std::invalid_argument("RAG result violates scope/filter/identity/bounds");}
}
}}
