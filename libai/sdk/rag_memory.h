#pragma once
#include "rag_index.h"
#include <mutex>
namespace libai {
// Bounded, ephemeral exact-cosine adapter for local use/tests. No durable DB claim.
class memory_vector_store_t:public vector_store_t {
    using records=std::map<std::string,vector_record_t>;
    std::map<std::pair<std::string,std::string>,records> spaces_;std::mutex mutex_;
    std::size_t max_records_,max_bytes_;
    static double cosine(const std::vector<double> &a,const std::vector<double> &b){
        if(a.size()!=b.size())throw std::invalid_argument("RAG query/index dimension mismatch");
        double sa=0,sb=0;for(std::size_t i=0;i<a.size();++i){sa=std::max(sa,std::abs(a[i]));sb=std::max(sb,std::abs(b[i]));}
        double na=0,nb=0,dot=0;for(std::size_t i=0;i<a.size();++i){double x=a[i]/sa,y=b[i]/sb;na+=x*x;nb+=y*y;dot+=x*y;}
        return std::max(-1.0,std::min(1.0,dot/std::sqrt(na)/std::sqrt(nb)));
    }
public:
    explicit memory_vector_store_t(std::size_t max_records=4096,std::size_t max_bytes=16*1024*1024):max_records_(max_records),max_bytes_(max_bytes){if(!max_records_||max_records_>4096||!max_bytes_||max_bytes_>64*1024*1024)throw std::invalid_argument("RAG memory capacity invalid");}
    void replace_documents(const std::vector<std::string> &documents,const std::vector<vector_record_t> &values,
        const std::string &space,const retrieval_context_t &context)override{
        // Validate even direct adapter use; commit_index is the portable boundary.
        validate_index_records(documents,values,space,context);std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);if(!lock.owns_lock())throw std::logic_error("RAG memory store busy");
        auto next=spaces_;auto key=std::make_pair(context.access_scope,space);auto &current=next[key];std::set<std::string> replaced(documents.begin(),documents.end());
        for(auto i=current.begin();i!=current.end();)if(replaced.count(i->second.chunk.document_id))i=current.erase(i);else ++i;
        for(const auto &r:values){auto old=current.find(r.chunk.id);if(old!=current.end()&&old->second.chunk.document_id!=r.chunk.document_id)throw std::invalid_argument("RAG chunk ID belongs to another document");current[r.chunk.id]=r;}
        std::size_t count=0,bytes=0;for(const auto &s:next){std::size_t dimension=0;for(const auto &r:s.second){if(dimension&&dimension!=r.second.vector.size())throw std::invalid_argument("RAG index dimension mismatch");dimension=r.second.vector.size();bytes+=rag_detail::chunk(r.second.chunk,rag_limits_t())+r.second.vector.size()*sizeof(double);++count;}}
        if(current.empty())next.erase(key);if(count>max_records_||bytes>max_bytes_||next.size()>64)throw std::invalid_argument("RAG memory index capacity exceeded");
        rag_detail::stopped(context);spaces_.swap(next);
    }
    retrieval_result_t search(const vector_query_t &query,const retrieval_context_t &context)override{
        rag_detail::request(query.request,context,rag_limits_t());rag_detail::utf8(query.embedding_space,128,false);rag_detail::vector(query.vector,rag_limits_t());
        std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);if(!lock.owns_lock())throw std::logic_error("RAG memory store busy");
        auto found=spaces_.find(std::make_pair(context.access_scope,query.embedding_space));retrieval_result_t result;
        if(found==spaces_.end()){for(const auto &s:spaces_)if(s.first.first==context.access_scope)throw std::invalid_argument("RAG embedding space not indexed");return result;}
        auto better=[](const retrieval_hit_t &a,const retrieval_hit_t &b){return a.score>b.score||(a.score==b.score&&a.chunk.id<b.chunk.id);};
        for(const auto &r:found->second){rag_detail::stopped(context);if(!rag_detail::matches(r.second.chunk.metadata_json,query.request.filter_json,16384))continue;
            retrieval_hit_t hit;hit.chunk=r.second.chunk;hit.access_scope=context.access_scope;hit.score=cosine(query.vector,r.second.vector);
            auto at=std::lower_bound(result.hits.begin(),result.hits.end(),hit,better);result.hits.insert(at,std::move(hit));
            if(result.hits.size()>query.request.top_k){result.hits.pop_back();result.truncated=true;}
        }return result;
    }
};
}
