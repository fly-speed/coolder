#pragma once
#include <libai/rag.h>
#include <set>
// Private C wire format. Metadata/filter retain the SDK's tighter JSON limits.
namespace rag_bridge {
namespace json=libai::rag_detail::json;
constexpr std::size_t wire_bytes=128*1024*1024,wire_nodes=5*1024*1024;
inline json::Document parse(const std::string &wire){json::Document d;libai::governance_detail::parse_json_bounded(wire,d,wire_bytes,wire_nodes);return d;}
inline void keys(const json::Value &d,std::initializer_list<const char*> allowed){
    if(!d.IsObject())throw std::invalid_argument("RAG expected object");
    std::set<std::string> names;for(auto k:allowed)names.insert(k);
    for(auto i=d.MemberBegin();i!=d.MemberEnd();++i)if(!names.count(std::string(i->name.GetString(),i->name.GetStringLength())))throw std::invalid_argument("RAG unknown field");
}
inline std::string string(const json::Value &v,std::size_t maximum){
    if(!v.IsString()||v.GetStringLength()>maximum)throw std::invalid_argument("RAG string field invalid");
    std::string s(v.GetString(),v.GetStringLength());libai::rag_detail::utf8(s,maximum);return s;
}
inline const json::Value &required(const json::Value &d,const char *k){if(!d.HasMember(k))throw std::invalid_argument("RAG required field missing");return d[k];}
inline std::string text(const json::Value &d,const char *k,std::size_t maximum,const char *fallback=nullptr){
    if(!d.HasMember(k)){if(fallback)return fallback;throw std::invalid_argument("RAG required string missing");}return string(d[k],maximum);
}
inline bool boolean(const json::Value &d,const char *k,bool fallback=false){if(!d.HasMember(k))return fallback;if(!d[k].IsBool())throw std::invalid_argument("RAG boolean invalid");return d[k].GetBool();}
inline std::size_t number(const json::Value &d,const char *k,std::size_t fallback,std::size_t maximum){
    if(!d.HasMember(k))return fallback;
    if(!d[k].IsNumber())throw std::invalid_argument("RAG numeric field invalid");double n=d[k].GetDouble();
    if(!std::isfinite(n)||n<0||n>maximum||n!=std::floor(n))throw std::invalid_argument("RAG numeric field out of range");return static_cast<std::size_t>(n);
}
inline void array(const json::Value &d,std::size_t maximum){if(!d.IsArray()||d.Size()>maximum)throw std::invalid_argument("RAG array count invalid");}
inline libai::rag_limits_t limits(const json::Value &d){
    keys(d,{"max_query_bytes","max_documents","max_chunks","max_hits","max_document_bytes","max_chunk_bytes","max_metadata_bytes","max_total_bytes","max_dimension"});libai::rag_limits_t l;
#define LIMIT(k) l.k=number(d,#k,l.k,l.k)
    LIMIT(max_query_bytes);LIMIT(max_documents);LIMIT(max_chunks);LIMIT(max_hits);LIMIT(max_document_bytes);LIMIT(max_chunk_bytes);LIMIT(max_metadata_bytes);LIMIT(max_total_bytes);LIMIT(max_dimension);
#undef LIMIT
    libai::rag_detail::limits(l);return l;
}
inline libai::rag_limits_t limits_field(const json::Value &d){return d.HasMember("limits")?limits(d["limits"]):libai::rag_limits_t();}
inline libai::document_t document(const json::Value &d){keys(d,{"id","source","revision","text","metadata_json"});libai::document_t v;
    v.id=text(d,"id",128);v.source=text(d,"source",2048);v.revision=text(d,"revision",128);v.text=text(d,"text",2*1024*1024);v.metadata_json=text(d,"metadata_json",16384,"{}");return v;}
inline libai::document_chunk_t chunk(const json::Value &d){keys(d,{"id","document_id","source","revision","text","metadata_json","has_span","begin","end"});libai::document_chunk_t v;
    v.id=text(d,"id",512);v.document_id=text(d,"document_id",128);v.source=text(d,"source",2048);v.revision=text(d,"revision",128);v.text=text(d,"text",65536);v.metadata_json=text(d,"metadata_json",16384,"{}");v.has_span=boolean(d,"has_span");
    if(v.has_span && (!d.HasMember("begin")||!d.HasMember("end")))throw std::invalid_argument("RAG span fields missing");
    if(!v.has_span && (d.HasMember("begin")||d.HasMember("end")))throw std::invalid_argument("RAG span fields require has_span");
    v.begin=number(d,"begin",0,2*1024*1024);v.end=number(d,"end",0,2*1024*1024);libai::rag_detail::chunk(v,libai::rag_limits_t());return v;}
inline std::vector<libai::document_t> documents(const json::Value &d){array(d,64);std::vector<libai::document_t> v;for(const auto &x:d.GetArray())v.push_back(document(x));libai::validate_documents(v);return v;}
inline std::vector<libai::document_chunk_t> chunks(const json::Value &d){array(d,256);std::vector<libai::document_chunk_t> v;for(const auto &x:d.GetArray())v.push_back(chunk(x));return v;}
inline std::vector<double> vector(const json::Value &d){array(d,16384);std::vector<double> v;for(const auto &x:d.GetArray()){if(!x.IsNumber())throw std::invalid_argument("RAG vector element invalid");v.push_back(x.GetDouble());}libai::rag_detail::vector(v,libai::rag_limits_t());return v;}
inline std::vector<std::string> strings(const json::Value &d,std::size_t count,std::size_t bytes){array(d,count);std::vector<std::string>v;for(const auto &x:d.GetArray())v.push_back(string(x,bytes));return v;}
inline libai::retrieval_request_t request(const json::Value &d){keys(d,{"query","filter_json","top_k"});libai::retrieval_request_t v;v.query=text(d,"query",8192);v.filter_json=text(d,"filter_json",16384,"{}");v.top_k=number(d,"top_k",8,64);return v;}
inline libai::retrieval_result_t result(const json::Value &d){keys(d,{"hits","truncated"});libai::retrieval_result_t v;v.truncated=boolean(d,"truncated");const auto &h=required(d,"hits");array(h,64);
    for(const auto &x:h.GetArray()){keys(x,{"chunk","access_scope","score"});libai::retrieval_hit_t hit;hit.chunk=chunk(required(x,"chunk"));hit.access_scope=text(x,"access_scope",128);const auto &score=required(x,"score");if(!score.IsNumber()||!std::isfinite(score.GetDouble()))throw std::invalid_argument("RAG score invalid");hit.score=score.GetDouble();v.hits.push_back(std::move(hit));}return v;}
inline libai::prepared_index_t batch(const json::Value &d){keys(d,{"documents","records","embedding_space"});libai::prepared_index_t v;v.documents=strings(required(d,"documents"),64,128);v.embedding_space=text(d,"embedding_space",128);const auto &r=required(d,"records");array(r,256);
    for(const auto &x:r.GetArray()){keys(x,{"chunk","vector"});libai::vector_record_t record;record.chunk=chunk(required(x,"chunk"));record.vector=vector(required(x,"vector"));v.records.push_back(std::move(record));}return v;}
inline void field(json::Value &d,const char *k,const std::string &v,json::Document::AllocatorType &a){libai::rag_detail::field(d,k,v,a);}
inline json::Value document_json(const libai::document_t &v,json::Document::AllocatorType &a){json::Value d(json::kObjectType);field(d,"id",v.id,a);field(d,"source",v.source,a);field(d,"revision",v.revision,a);field(d,"text",v.text,a);field(d,"metadata_json",v.metadata_json,a);return d;}
inline json::Value chunk_json(const libai::document_chunk_t &v,json::Document::AllocatorType &a){json::Value d(json::kObjectType);field(d,"id",v.id,a);field(d,"document_id",v.document_id,a);field(d,"source",v.source,a);field(d,"revision",v.revision,a);field(d,"text",v.text,a);field(d,"metadata_json",v.metadata_json,a);d.AddMember("has_span",v.has_span,a);if(v.has_span){d.AddMember("begin",static_cast<uint64_t>(v.begin),a);d.AddMember("end",static_cast<uint64_t>(v.end),a);}return d;}
inline json::Value strings_json(const std::vector<std::string> &v,json::Document::AllocatorType &a){json::Value d(json::kArrayType);for(const auto &s:v){json::Value x;x.SetString(s.data(),static_cast<json::SizeType>(s.size()),a);d.PushBack(x,a);}return d;}
inline json::Value vector_json(const std::vector<double> &v,json::Document::AllocatorType &a){json::Value d(json::kArrayType);for(double s:v)d.PushBack(s,a);return d;}
inline json::Value request_json(const libai::retrieval_request_t &v,json::Document::AllocatorType &a){json::Value d(json::kObjectType);field(d,"query",v.query,a);field(d,"filter_json",v.filter_json,a);d.AddMember("top_k",static_cast<uint64_t>(v.top_k),a);return d;}
inline json::Document documents_json(const std::vector<libai::document_t> &v){json::Document d;d.SetArray();for(const auto &x:v)d.PushBack(document_json(x,d.GetAllocator()),d.GetAllocator());return d;}
inline json::Document result_json(const libai::retrieval_result_t &v){json::Document d;d.SetObject();auto &a=d.GetAllocator();d.AddMember("truncated",v.truncated,a);json::Value hits(json::kArrayType);for(const auto &h:v.hits){json::Value x(json::kObjectType);x.AddMember("chunk",chunk_json(h.chunk,a),a);field(x,"access_scope",h.access_scope,a);x.AddMember("score",h.score,a);hits.PushBack(x,a);}d.AddMember("hits",hits,a);return d;}
inline json::Document batch_json(const libai::prepared_index_t &v){json::Document d;d.SetObject();auto &a=d.GetAllocator();d.AddMember("documents",strings_json(v.documents,a),a);field(d,"embedding_space",v.embedding_space,a);json::Value records(json::kArrayType);for(const auto &r:v.records){json::Value x(json::kObjectType);x.AddMember("chunk",chunk_json(r.chunk,a),a);x.AddMember("vector",vector_json(r.vector,a),a);records.PushBack(x,a);}d.AddMember("records",records,a);return d;}
inline std::string wire(const json::Value &d){auto v=libai::rag_detail::wire(d);if(v.size()>wire_bytes)throw std::invalid_argument("RAG wire byte limit exceeded");return v;}
}
