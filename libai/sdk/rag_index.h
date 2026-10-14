#pragma once
#include "rag_validation.h"
namespace libai {
inline void validate_documents(const std::vector<document_t> &docs,const rag_limits_t &limits=rag_limits_t()){
    rag_detail::limits(limits);if(docs.size()>limits.max_documents)throw std::invalid_argument("RAG document count exceeded");
    std::set<std::string> ids;std::size_t bytes=0;for(const auto &d:docs){bytes+=rag_detail::document(d,limits);if(bytes>limits.max_total_bytes||!ids.insert(d.id).second)throw std::invalid_argument("RAG document bytes/duplicate ID");}
}
inline std::vector<document_t> load_documents(document_loader_t &loader,const std::string &source,
    const retrieval_context_t &context,const rag_limits_t &limits=rag_limits_t()){
    rag_detail::limits(limits);rag_detail::scope(context);rag_detail::utf8(source,2048,false);
    auto result=loader.load(source,context);rag_detail::stopped(context);validate_documents(result,limits);return result;
}
// Fixed UTF-8 byte windows with explicit overlap; no tokenizer or vendor dependency.
class utf8_text_splitter_t:public text_splitter_t {
    std::size_t size_,overlap_;rag_limits_t limits_;
public:
    explicit utf8_text_splitter_t(std::size_t bytes=1024,std::size_t overlap=128,rag_limits_t limits=rag_limits_t()):size_(bytes),overlap_(overlap),limits_(limits){
        rag_detail::limits(limits_);if(size_<4||size_>limits_.max_chunk_bytes||overlap_>=size_)throw std::invalid_argument("RAG splitter size/overlap invalid");}
    std::vector<document_chunk_t> split(const std::vector<document_t> &docs,const retrieval_context_t &context)override{
        rag_detail::scope(context);validate_documents(docs,limits_);std::vector<document_chunk_t> result;std::size_t total=0;
        for(const auto &doc:docs){for(std::size_t begin=0;begin<doc.text.size();){rag_detail::stopped(context);
            auto end=std::min(begin+size_,doc.text.size());while(end<doc.text.size()&&(static_cast<unsigned char>(doc.text[end])&0xc0)==0x80)--end;
            if(end<=begin)throw std::invalid_argument("RAG window cannot fit a UTF-8 codepoint");
            document_chunk_t c;c.id=doc.id+"#"+std::to_string(begin)+"-"+std::to_string(end)+"@"+doc.revision;
            c.document_id=doc.id;c.source=doc.source;c.revision=doc.revision;c.text=doc.text.substr(begin,end-begin);c.metadata_json=doc.metadata_json;c.has_span=true;c.begin=begin;c.end=end;
            total+=rag_detail::chunk(c,limits_);if(total>limits_.max_total_bytes||result.size()>=limits_.max_chunks)throw std::invalid_argument("RAG split count/bytes exceeded");result.push_back(std::move(c));
            if(end==doc.text.size())break;auto next=end-std::min(overlap_,end-begin-1);while(next<end&&(static_cast<unsigned char>(doc.text[next])&0xc0)==0x80)++next;
            begin=next;
        }}return result;
    }
};
// No vector store writes here. Callers can persist/review this prepared batch first.
inline prepared_index_t prepare_index(const std::vector<document_t> &docs,text_splitter_t &splitter,
    embedding_model_t &embedding,const retrieval_context_t &context,const rag_limits_t &limits=rag_limits_t()){
    rag_detail::limits(limits);rag_detail::scope(context);validate_documents(docs,limits);prepared_index_t batch;
    std::map<std::string,const document_t*> sources;for(const auto &d:docs){sources[d.id]=&d;batch.documents.push_back(d.id);}
    auto chunks=splitter.split(docs,context);rag_detail::stopped(context);
    if(chunks.size()>limits.max_chunks)throw std::invalid_argument("RAG chunk count exceeded");
    std::set<std::string> ids;std::size_t total=0;std::vector<std::string> texts;
    for(const auto &c:chunks){total+=rag_detail::chunk(c,limits);auto found=sources.find(c.document_id);
        if(total>limits.max_total_bytes||!ids.insert(c.id).second||found==sources.end())throw std::invalid_argument("RAG splitter returned unknown/duplicate/oversized chunks");
        const auto &doc=*found->second;
        if(c.source!=doc.source||c.revision!=doc.revision||rag_detail::object(c.metadata_json,limits.max_metadata_bytes)!=rag_detail::object(doc.metadata_json,limits.max_metadata_bytes)||
            (c.has_span&&(c.end>doc.text.size()||doc.text.substr(c.begin,c.end-c.begin)!=c.text)))throw std::invalid_argument("RAG splitter changed provenance or source span");
        texts.push_back(c.text);
    }
    batch.embedding_space=embedding.space_id();rag_detail::utf8(batch.embedding_space,128,false);
    auto vectors=texts.empty()?std::vector<std::vector<double>>():embedding.embed(texts,context);rag_detail::stopped(context);
    if(embedding.space_id()!=batch.embedding_space||vectors.size()!=chunks.size())throw std::invalid_argument("RAG embedding space/count changed");
    std::size_t dimension=0;for(std::size_t i=0;i<vectors.size();++i){rag_detail::vector(vectors[i],limits);if(dimension&&dimension!=vectors[i].size())throw std::invalid_argument("RAG mixed dimensions");dimension=vectors[i].size();vector_record_t r;r.chunk=std::move(chunks[i]);r.vector=std::move(vectors[i]);batch.records.push_back(std::move(r));}
    return batch;
}
// Explicit external side effect; adapters own durable atomicity and receipts.
inline void validate_index_records(const std::vector<std::string> &documents,const std::vector<vector_record_t> &records,
    const std::string &space,const retrieval_context_t &context,const rag_limits_t &limits=rag_limits_t()){
    rag_detail::limits(limits);rag_detail::scope(context);rag_detail::utf8(space,128,false);
    if(documents.size()>limits.max_documents||records.size()>limits.max_chunks)throw std::invalid_argument("RAG index batch exceeds count");
    std::set<std::string> docs,ids;for(const auto &d:documents){rag_detail::utf8(d,128,false);if(!docs.insert(d).second)throw std::invalid_argument("RAG duplicate replacement document");}
    std::size_t total=0,dimension=0;for(const auto &r:records){total+=rag_detail::chunk(r.chunk,limits);rag_detail::vector(r.vector,limits);
        if(total>limits.max_total_bytes||!docs.count(r.chunk.document_id)||!ids.insert(r.chunk.id).second||(dimension&&dimension!=r.vector.size()))throw std::invalid_argument("RAG invalid replacement record");dimension=r.vector.size();}
    rag_detail::stopped(context);
}
inline void commit_index(vector_store_t &store,const prepared_index_t &batch,const retrieval_context_t &context,
    const rag_limits_t &limits=rag_limits_t()){
    validate_index_records(batch.documents,batch.records,batch.embedding_space,context,limits);
    store.replace_documents(batch.documents,batch.records,batch.embedding_space,context);rag_detail::stopped(context);
}
inline retrieval_result_t retrieve(retriever_t &retriever,const retrieval_request_t &request,
    const retrieval_context_t &context,const rag_limits_t &limits=rag_limits_t()){
    rag_detail::request(request,context,limits);auto result=retriever.search(request,context);rag_detail::stopped(context);rag_detail::hits(result,request,context,limits);return result;
}
class vector_retriever_t:public retriever_t {
    std::shared_ptr<embedding_model_t> embedding_;std::shared_ptr<vector_store_t> store_;rag_limits_t limits_;
public:
    vector_retriever_t(std::shared_ptr<embedding_model_t> embedding,std::shared_ptr<vector_store_t> store,rag_limits_t limits=rag_limits_t()):embedding_(std::move(embedding)),store_(std::move(store)),limits_(limits){rag_detail::limits(limits_);if(!embedding_||!store_)throw std::invalid_argument("RAG vector adapters required");}
    retrieval_result_t search(const retrieval_request_t &request,const retrieval_context_t &context)override{
        rag_detail::request(request,context,limits_);vector_query_t query;query.request=request;query.embedding_space=embedding_->space_id();rag_detail::utf8(query.embedding_space,128,false);
        auto vectors=embedding_->embed({request.query},context);rag_detail::stopped(context);if(vectors.size()!=1||embedding_->space_id()!=query.embedding_space)throw std::invalid_argument("RAG query embedding count/space changed");rag_detail::vector(vectors[0],limits_);query.vector=std::move(vectors[0]);
        auto result=store_->search(query,context);rag_detail::stopped(context);rag_detail::hits(result,request,context,limits_);return result;
    }
};
}
