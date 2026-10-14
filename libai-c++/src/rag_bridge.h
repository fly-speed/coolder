#pragma once
#include <libai_cpp/rag.h>
#include "value_bridge.h"
#include <libai/rag.h>
namespace libai { namespace cpp {
struct Rag::Bridge {
    static ::libai::rag_limits_t limits(const RagLimits &s){::libai::rag_limits_t t;
        t.max_query_bytes=s.max_query_bytes;t.max_documents=s.max_documents;t.max_chunks=s.max_chunks;t.max_hits=s.max_hits;
        t.max_document_bytes=s.max_document_bytes;t.max_chunk_bytes=s.max_chunk_bytes;t.max_metadata_bytes=s.max_metadata_bytes;t.max_total_bytes=s.max_total_bytes;t.max_dimension=s.max_dimension;return t;}
    static ::libai::retrieval_context_t context(const RetrievalContext &s){::libai::retrieval_context_t t;t.access_scope=s.access_scope;t.control.deadline=s.control.deadline;t.control.idempotency_key=s.control.idempotency_key;if(s.control.cancellation)t.control.cancellation=s.control.cancellation->impl_->native;return t;}
    static RetrievalContext context(const ::libai::retrieval_context_t &s){RetrievalContext t;t.access_scope=s.access_scope;t.control.deadline=s.control.deadline;t.control.idempotency_key=s.control.idempotency_key;if(s.control.cancellation){t.control.cancellation=std::make_shared<Cancellation>();t.control.cancellation->impl_->native=s.control.cancellation;}return t;}
    template<class S>static void text_bounds(const S&s,std::size_t text_max){
        if(s.id.size()>512||s.source.size()>2048||s.revision.size()>128||s.text.size()>text_max||s.metadata_json.size()>16384)throw std::invalid_argument("RAG value exceeds conversion bounds");
    }
    template<class S>static std::size_t weight(const S&s){return s.text.size()+s.metadata_json.size()+s.id.size()+s.source.size()+s.revision.size();}
    static std::size_t weight(const VectorRecord&s){if(s.vector.size()>16384)throw std::invalid_argument("RAG vector exceeds conversion bounds");return weight(s.chunk);}
    static std::size_t weight(const ::libai::vector_record_t&s){if(s.vector.size()>16384)throw std::invalid_argument("RAG vector exceeds conversion bounds");return weight(s.chunk);}
    template<class T,class S>static T document(const S&s){text_bounds(s,2*1024*1024);T t;t.id=s.id;t.source=s.source;t.revision=s.revision;t.text=s.text;t.metadata_json=s.metadata_json;return t;}
    template<class T,class S>static T chunk(const S&s){text_bounds(s,65536);if(s.document_id.size()>128)throw std::invalid_argument("RAG document ID exceeds conversion bounds");T t;t.id=s.id;t.document_id=s.document_id;t.source=s.source;t.revision=s.revision;t.text=s.text;t.metadata_json=s.metadata_json;t.has_span=s.has_span;t.begin=s.begin;t.end=s.end;return t;}
    template<class T,class S>static T request(const S&s){if(s.query.size()>8192||s.filter_json.size()>16384)throw std::invalid_argument("RAG request exceeds conversion bounds");T t;t.query=s.query;t.filter_json=s.filter_json;t.top_k=s.top_k;return t;}
    template<class T,class S>static T hit(const S&s){T t;t.chunk=chunk<decltype(t.chunk)>(s.chunk);t.access_scope=s.access_scope;t.score=s.score;return t;}
    template<class T,class S>static T result(const S&s){if(s.hits.size()>64)throw std::invalid_argument("RAG hit count exceeds conversion bounds");std::size_t bytes=0;for(const auto &h:s.hits){bytes+=weight(h.chunk);if(bytes>4*1024*1024||h.access_scope.size()>128)throw std::invalid_argument("RAG hit data exceeds conversion bounds");}T t;t.truncated=s.truncated;for(const auto &h:s.hits)t.hits.push_back(hit<typename decltype(t.hits)::value_type>(h));return t;}
    template<class T,class S>static T record(const S&s){if(s.vector.size()>16384)throw std::invalid_argument("RAG vector exceeds conversion bounds");T t;t.chunk=chunk<decltype(t.chunk)>(s.chunk);t.vector=s.vector;return t;}
    template<class T,class S>static T batch(const S&s){if(s.documents.size()>64||s.records.size()>256||s.embedding_space.size()>128)throw std::invalid_argument("RAG index batch exceeds conversion bounds");std::size_t bytes=0;for(const auto &r:s.records){bytes+=weight(r);if(bytes>4*1024*1024)throw std::invalid_argument("RAG index data exceeds conversion bounds");}T t;t.documents=s.documents;t.embedding_space=s.embedding_space;for(const auto &r:s.records)t.records.push_back(record<typename decltype(t.records)::value_type>(r));return t;}
    template<class T,class S>static T query(const S&s){if(s.vector.size()>16384||s.embedding_space.size()>128)throw std::invalid_argument("RAG query exceeds conversion bounds");T t;t.request=request<decltype(t.request)>(s.request);t.embedding_space=s.embedding_space;t.vector=s.vector;return t;}
    template<class T,class S,class F>static std::vector<T> values(const std::vector<S>&source,F convert){if(source.size()>256)throw std::invalid_argument("RAG conversion count exceeds limit");std::size_t bytes=0;for(const auto &s:source){bytes+=weight(s);if(bytes>4*1024*1024)throw std::invalid_argument("RAG conversion data exceeds limit");}std::vector<T> target;target.reserve(source.size());for(const auto &s:source)target.push_back(convert(s));return target;}
    class LoaderAdapter:public ::libai::document_loader_t {
        DocumentLoader &owner_;
    public:explicit LoaderAdapter(DocumentLoader &owner):owner_(owner){}
        std::vector<::libai::document_t> load(const std::string &source,const ::libai::retrieval_context_t &c)override{return values<::libai::document_t>(owner_.load(source,context(c)),document<::libai::document_t,Document>);}
    };
    class SplitterAdapter:public ::libai::text_splitter_t {
        TextSplitter &owner_;
    public:explicit SplitterAdapter(TextSplitter &owner):owner_(owner){}
        std::vector<::libai::document_chunk_t> split(const std::vector<::libai::document_t>&d,const ::libai::retrieval_context_t &c)override{return values<::libai::document_chunk_t>(owner_.split(values<Document>(d,document<Document,::libai::document_t>),context(c)),chunk<::libai::document_chunk_t,DocumentChunk>);}
    };
    class EmbeddingAdapter:public ::libai::embedding_model_t {
        std::shared_ptr<EmbeddingModel> owner_;
    public:explicit EmbeddingAdapter(std::shared_ptr<EmbeddingModel> owner):owner_(std::move(owner)){if(!owner_)throw std::invalid_argument("embedding model required");}
        std::string space_id()const override{return owner_->space_id();}
        std::vector<std::vector<double>> embed(const std::vector<std::string>&text,const ::libai::retrieval_context_t &c)override{return owner_->embed(text,context(c));}
    };
    class StoreAdapter:public ::libai::vector_store_t {
        std::shared_ptr<VectorStore> owner_;
    public:explicit StoreAdapter(std::shared_ptr<VectorStore> owner):owner_(std::move(owner)){if(!owner_)throw std::invalid_argument("vector store required");}
        void replace_documents(const std::vector<std::string>&d,const std::vector<::libai::vector_record_t>&r,const std::string&s,const ::libai::retrieval_context_t &c)override{owner_->replace_documents(d,values<VectorRecord>(r,record<VectorRecord,::libai::vector_record_t>),s,context(c));}
        ::libai::retrieval_result_t search(const ::libai::vector_query_t &q,const ::libai::retrieval_context_t &c)override{return result<::libai::retrieval_result_t>(owner_->search(query<VectorQuery>(q),context(c)));}
    };
    class RetrieverAdapter:public ::libai::retriever_t {
        std::shared_ptr<Retriever> owner_;
    public:explicit RetrieverAdapter(std::shared_ptr<Retriever> owner):owner_(std::move(owner)){if(!owner_)throw std::invalid_argument("retriever required");}
        ::libai::retrieval_result_t search(const ::libai::retrieval_request_t &q,const ::libai::retrieval_context_t &c)override{return result<::libai::retrieval_result_t>(owner_->search(request<RetrievalRequest>(q),context(c)));}
    };
    static ::libai::rag_binding_t binding(const RagBinding &s){::libai::rag_binding_t t;t.execution_revision=s.execution_revision;t.access_scope=s.access_scope;t.filter_json=s.filter_json;t.requires_approval=s.requires_approval;t.side_effect_free=s.side_effect_free;t.max_top_k=s.max_top_k;t.max_result_bytes=s.max_result_bytes;t.limits=limits(s.limits);return t;}
};
}}
