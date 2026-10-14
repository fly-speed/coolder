#include "rag_bridge.h"
namespace libai { namespace cpp {
FunctionRetriever::FunctionRetriever(RetrievalFunction callback):callback_(std::move(callback)){if(!callback_)throw std::invalid_argument("retriever callback required");}
FunctionRetriever::~FunctionRetriever()=default;
RetrievalResult FunctionRetriever::search(const RetrievalRequest &r,const RetrievalContext &c){return callback_(r,c);}
void Rag::validate_documents(const std::vector<Document>&docs,const RagLimits &limits){::libai::validate_documents(Bridge::values<::libai::document_t>(docs,Bridge::document<::libai::document_t,Document>),Bridge::limits(limits));}
std::vector<Document> Rag::load_documents(DocumentLoader &loader,const std::string &source,const RetrievalContext &c,const RagLimits &limits){Bridge::LoaderAdapter adapter(loader);return Bridge::values<Document>(::libai::load_documents(adapter,source,Bridge::context(c),Bridge::limits(limits)),Bridge::document<Document,::libai::document_t>);}
PreparedIndex Rag::prepare_index(const std::vector<Document> &docs,TextSplitter &splitter,EmbeddingModel &embedding,const RetrievalContext &c,const RagLimits &limits){
    Bridge::SplitterAdapter s(splitter);Bridge::EmbeddingAdapter e(std::shared_ptr<EmbeddingModel>(&embedding,[](EmbeddingModel*){}));
    return Bridge::batch<PreparedIndex>(::libai::prepare_index(Bridge::values<::libai::document_t>(docs,Bridge::document<::libai::document_t,Document>),s,e,Bridge::context(c),Bridge::limits(limits)));
}
void Rag::commit_index(VectorStore &store,const PreparedIndex &batch,const RetrievalContext &c,const RagLimits &limits){Bridge::StoreAdapter s(std::shared_ptr<VectorStore>(&store,[](VectorStore*){}));::libai::commit_index(s,Bridge::batch<::libai::prepared_index_t>(batch),Bridge::context(c),Bridge::limits(limits));}
RetrievalResult Rag::retrieve(Retriever &retriever,const RetrievalRequest &q,const RetrievalContext &c,const RagLimits &limits){Bridge::RetrieverAdapter r(std::shared_ptr<Retriever>(&retriever,[](Retriever*){}));return Bridge::result<RetrievalResult>(::libai::retrieve(r,Bridge::request<::libai::retrieval_request_t>(q),Bridge::context(c),Bridge::limits(limits)));}
struct Utf8TextSplitter::Impl {::libai::utf8_text_splitter_t native;Impl(std::size_t s,std::size_t o,const ::libai::rag_limits_t &l):native(s,o,l){}};
Utf8TextSplitter::Utf8TextSplitter(std::size_t bytes,std::size_t overlap,const RagLimits &l):impl_(new Impl(bytes,overlap,Rag::Bridge::limits(l))){}
Utf8TextSplitter::~Utf8TextSplitter()=default;
std::vector<DocumentChunk> Utf8TextSplitter::split(const std::vector<Document>&d,const RetrievalContext &c){return Rag::Bridge::values<DocumentChunk>(impl_->native.split(Rag::Bridge::values<::libai::document_t>(d,Rag::Bridge::document<::libai::document_t,Document>),Rag::Bridge::context(c)),Rag::Bridge::chunk<DocumentChunk,::libai::document_chunk_t>);}
struct MemoryVectorStore::Impl {::libai::memory_vector_store_t native;Impl(std::size_t r,std::size_t b):native(r,b){}};
MemoryVectorStore::MemoryVectorStore(std::size_t records,std::size_t bytes):impl_(new Impl(records,bytes)){}
MemoryVectorStore::~MemoryVectorStore()=default;
void MemoryVectorStore::replace_documents(const std::vector<std::string> &docs,const std::vector<VectorRecord>&records,const std::string &space,const RetrievalContext &c){impl_->native.replace_documents(docs,Rag::Bridge::values<::libai::vector_record_t>(records,Rag::Bridge::record<::libai::vector_record_t,VectorRecord>),space,Rag::Bridge::context(c));}
RetrievalResult MemoryVectorStore::search(const VectorQuery &q,const RetrievalContext &c){return Rag::Bridge::result<RetrievalResult>(impl_->native.search(Rag::Bridge::query<::libai::vector_query_t>(q),Rag::Bridge::context(c)));}
struct VectorRetriever::Impl {::libai::vector_retriever_t native;Impl(std::shared_ptr<EmbeddingModel> e,std::shared_ptr<VectorStore> s,const RagLimits &l):native(std::make_shared<Rag::Bridge::EmbeddingAdapter>(std::move(e)),std::make_shared<Rag::Bridge::StoreAdapter>(std::move(s)),Rag::Bridge::limits(l)){}};
VectorRetriever::VectorRetriever(std::shared_ptr<EmbeddingModel> e,std::shared_ptr<VectorStore>s,const RagLimits &l):impl_(new Impl(std::move(e),std::move(s),l)){}
VectorRetriever::~VectorRetriever()=default;
RetrievalResult VectorRetriever::search(const RetrievalRequest &r,const RetrievalContext &c){return Rag::Bridge::result<RetrievalResult>(impl_->native.search(Rag::Bridge::request<::libai::retrieval_request_t>(r),Rag::Bridge::context(c)));}
}}
