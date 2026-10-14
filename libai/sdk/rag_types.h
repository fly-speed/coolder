#pragma once
#include "agent_types.h"
namespace libai {
// Host-owned source revision and metadata, never proof of truth supplied by a model.
struct document_t {std::string id,source,revision,text,metadata_json="{}";};
struct document_chunk_t {
    std::string id,document_id,source,revision,text,metadata_json="{}";
    bool has_span=false;
    // UTF-8 byte positions in the original source document, not character offsets.
    std::size_t begin=0,end=0;
};
struct retrieval_context_t {
    tool_context_t control;
    retrieval_context_t(){control.deadline=std::chrono::steady_clock::time_point::max();}
    // Trusted tenant/index namespace. Adapters must enforce their own access policy.
    std::string access_scope;
    bool stopped()const{return control.stopped();}
};
struct retrieval_request_t {std::string query,filter_json="{}";std::size_t top_k=8;};
struct retrieval_hit_t {document_chunk_t chunk;std::string access_scope;double score=0;};
struct retrieval_result_t {std::vector<retrieval_hit_t> hits;bool truncated=false;};
struct rag_limits_t {
    std::size_t max_query_bytes=8192,max_documents=64,max_chunks=256,max_hits=64;
    std::size_t max_document_bytes=2*1024*1024,max_chunk_bytes=65536,max_metadata_bytes=16384;
    std::size_t max_total_bytes=4*1024*1024,max_dimension=16384;
};
// Source resolution and document decoding are explicit trusted host operations.
class document_loader_t {public:virtual ~document_loader_t()=default;
    virtual std::vector<document_t> load(const std::string &source,const retrieval_context_t &)=0;};
class text_splitter_t {public:virtual ~text_splitter_t()=default;
    virtual std::vector<document_chunk_t> split(const std::vector<document_t>&,const retrieval_context_t &)=0;};
// Identifies model/normalization/dimension semantics. Change when vector meaning changes.
class embedding_model_t {public:virtual ~embedding_model_t()=default;
    virtual std::string space_id()const=0;
    virtual std::vector<std::vector<double>> embed(const std::vector<std::string>&,const retrieval_context_t &)=0;};
struct vector_record_t {document_chunk_t chunk;std::vector<double> vector;};
struct vector_query_t {retrieval_request_t request;std::string embedding_space;std::vector<double> vector;};
class vector_store_t {public:virtual ~vector_store_t()=default;
    // Replace the complete chunk set of specified document IDs in this scope/space.
    // Host stores own durable atomicity/deduplication; failures can leave unknown effects.
    virtual void replace_documents(const std::vector<std::string>&,const std::vector<vector_record_t>&,
        const std::string &embedding_space,const retrieval_context_t &)=0;
    virtual retrieval_result_t search(const vector_query_t&,const retrieval_context_t &)=0;};
// Can implement SQL/full-text/vector/hybrid retrieval without using an embedding API.
class retriever_t {public:virtual ~retriever_t()=default;
    virtual retrieval_result_t search(const retrieval_request_t&,const retrieval_context_t &)=0;};
class function_retriever_t:public retriever_t {
    std::function<retrieval_result_t(const retrieval_request_t&,const retrieval_context_t&)> callback_;
public:
    explicit function_retriever_t(decltype(callback_) callback):callback_(std::move(callback)){
        if(!callback_)throw std::invalid_argument("retriever callback empty");}
    retrieval_result_t search(const retrieval_request_t &request,const retrieval_context_t &context)override{return callback_(request,context);}
};
struct prepared_index_t {std::vector<std::string> documents;std::vector<vector_record_t> records;std::string embedding_space;};
}
