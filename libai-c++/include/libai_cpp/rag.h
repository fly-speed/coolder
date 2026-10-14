#pragma once
#include "types.h"
namespace libai { namespace cpp {
// Independent source and citation values; metadata remains portable JSON text.
struct Document {std::string id,source,revision,text,metadata_json="{}";};
struct DocumentChunk {std::string id,document_id,source,revision,text,metadata_json="{}";bool has_span=false;std::size_t begin=0,end=0;};
struct RetrievalContext {
    ToolContext control;
    std::string access_scope;
    RetrievalContext(){control.deadline=std::chrono::steady_clock::time_point::max();}
    bool stopped()const{return control.stopped();}
};
struct RetrievalRequest {std::string query,filter_json="{}";std::size_t top_k=8;};
struct RetrievalHit {DocumentChunk chunk;std::string access_scope;double score=0;};
struct RetrievalResult {std::vector<RetrievalHit> hits;bool truncated=false;};
struct RagLimits {
    std::size_t max_query_bytes=8192,max_documents=64,max_chunks=256,max_hits=64;
    std::size_t max_document_bytes=2*1024*1024,max_chunk_bytes=65536,max_metadata_bytes=16384;
    std::size_t max_total_bytes=4*1024*1024,max_dimension=16384;
};
class LIBAI_CPP_API DocumentLoader {public:virtual ~DocumentLoader(){};
    virtual std::vector<Document> load(const std::string&,const RetrievalContext&)=0;};
class LIBAI_CPP_API TextSplitter {public:virtual ~TextSplitter(){};
    virtual std::vector<DocumentChunk> split(const std::vector<Document>&,const RetrievalContext&)=0;};
class LIBAI_CPP_API EmbeddingModel {public:virtual ~EmbeddingModel(){};
    virtual std::string space_id()const=0;
    virtual std::vector<std::vector<double>> embed(const std::vector<std::string>&,const RetrievalContext&)=0;};
struct VectorRecord {DocumentChunk chunk;std::vector<double> vector;};
struct VectorQuery {RetrievalRequest request;std::string embedding_space;std::vector<double> vector;};
class LIBAI_CPP_API VectorStore {public:virtual ~VectorStore(){};
    // Explicit side effect; host owns durable atomicity/deduplication/receipts.
    virtual void replace_documents(const std::vector<std::string>&,const std::vector<VectorRecord>&,
        const std::string&,const RetrievalContext&)=0;
    virtual RetrievalResult search(const VectorQuery&,const RetrievalContext&)=0;};
class LIBAI_CPP_API Retriever {public:virtual ~Retriever(){};
    virtual RetrievalResult search(const RetrievalRequest&,const RetrievalContext&)=0;};
using RetrievalFunction=std::function<RetrievalResult(const RetrievalRequest&,const RetrievalContext&)>;
class LIBAI_CPP_API FunctionRetriever:public Retriever {
public:explicit FunctionRetriever(RetrievalFunction);~FunctionRetriever();
    RetrievalResult search(const RetrievalRequest&,const RetrievalContext&)override;
private:RetrievalFunction callback_;
};
struct PreparedIndex {std::vector<std::string> documents;std::vector<VectorRecord> records;std::string embedding_space;};
struct RagBinding {
    // Host version MUST cover corpus/backend/embedding/ACL/filter changes.
    std::string execution_revision,access_scope,filter_json="{}";
    bool requires_approval=true,side_effect_free=false;
    std::size_t max_top_k=8,max_result_bytes=32768;
    RagLimits limits;
};
class LIBAI_CPP_API Rag {
public:
    static void validate_documents(const std::vector<Document>&,const RagLimits & =RagLimits());
    static std::vector<Document> load_documents(DocumentLoader&,const std::string&,const RetrievalContext&,const RagLimits & =RagLimits());
    static PreparedIndex prepare_index(const std::vector<Document>&,TextSplitter&,EmbeddingModel&,const RetrievalContext&,const RagLimits & =RagLimits());
    static void commit_index(VectorStore&,const PreparedIndex&,const RetrievalContext&,const RagLimits & =RagLimits());
    static RetrievalResult retrieve(Retriever&,const RetrievalRequest&,const RetrievalContext&,const RagLimits & =RagLimits());
private:
    friend class Agent;friend class Utf8TextSplitter;friend class MemoryVectorStore;friend class VectorRetriever;
    struct Bridge;
};
class LIBAI_CPP_API Utf8TextSplitter:public TextSplitter {
public:explicit Utf8TextSplitter(std::size_t bytes=1024,std::size_t overlap=128,const RagLimits & =RagLimits());
    ~Utf8TextSplitter();Utf8TextSplitter(const Utf8TextSplitter&)=delete;Utf8TextSplitter &operator=(const Utf8TextSplitter&)=delete;
    std::vector<DocumentChunk> split(const std::vector<Document>&,const RetrievalContext&)override;
private:struct Impl;std::unique_ptr<Impl> impl_;
};
class LIBAI_CPP_API MemoryVectorStore:public VectorStore {
public:explicit MemoryVectorStore(std::size_t records=4096,std::size_t bytes=16*1024*1024);
    ~MemoryVectorStore();MemoryVectorStore(const MemoryVectorStore&)=delete;MemoryVectorStore &operator=(const MemoryVectorStore&)=delete;
    void replace_documents(const std::vector<std::string>&,const std::vector<VectorRecord>&,const std::string&,const RetrievalContext&)override;
    RetrievalResult search(const VectorQuery&,const RetrievalContext&)override;
private:struct Impl;std::unique_ptr<Impl> impl_;
};
class LIBAI_CPP_API VectorRetriever:public Retriever {
public:VectorRetriever(std::shared_ptr<EmbeddingModel>,std::shared_ptr<VectorStore>,const RagLimits & =RagLimits());
    ~VectorRetriever();VectorRetriever(const VectorRetriever&)=delete;VectorRetriever &operator=(const VectorRetriever&)=delete;
    RetrievalResult search(const RetrievalRequest&,const RetrievalContext&)override;
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}}
