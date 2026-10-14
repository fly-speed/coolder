#include "bridge_internal.h"
#include "rag_json.h"
#include <cstring>
struct libai_rag_result {std::string wire,error;bool set=false,failed=false;};
struct libai_rag {
    std::mutex mutex;
    std::shared_ptr<std::atomic<bool>> callback_busy;
    libai::rag_limits_t limits;
    std::shared_ptr<libai::document_loader_t> loader;
    std::shared_ptr<libai::text_splitter_t> splitter;
    std::shared_ptr<libai::embedding_model_t> embedding;
    std::shared_ptr<libai::vector_store_t> store;
    std::shared_ptr<libai::retriever_t> retriever;
};
namespace {
using namespace rag_bridge;
std::unique_lock<std::mutex> lock(libai_rag *h){if(!h)throw std::invalid_argument("RAG handle null");std::unique_lock<std::mutex> g(h->mutex,std::try_to_lock);if(!g.owns_lock()||(h->callback_busy&&h->callback_busy->load()))throw std::logic_error("RAG handle busy");return g;}
libai::retrieval_context_t context(const char *wire,const libai_cancellation *token){
    auto d=parse(wire?bridge_text(wire):"{}");keys(d,{"access_scope","timeout_ms","idempotency_key"});libai::retrieval_context_t c;c.access_scope=text(d,"access_scope",128);c.control.idempotency_key=text(d,"idempotency_key",4096,"");
    auto ms=number(d,"timeout_ms",300000,86400000);if(ms)c.control.deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(ms);
    c.control.cancellation=token?token->value:std::make_shared<libai::cancellation_t>();libai::rag_detail::scope(c);return c;
}
struct Callback {
    std::shared_ptr<std::atomic<bool>> busy=std::make_shared<std::atomic<bool>>(false);libai::rag_limits_t limits;std::mutex mutex;libai_rag_fn fn;void *user;libai_rag_release_fn release;bool active=false;
    Callback(libai_rag_fn f,void *u,libai_rag_release_fn r):fn(f),user(u),release(r){}
    ~Callback(){if(active&&release)try{release(user);}catch(...){}}
    json::Document call(const char *op,const json::Value &arguments,const libai::retrieval_context_t *c){
        std::unique_lock<std::mutex> guard(mutex,std::try_to_lock);if(!guard.owns_lock())throw std::logic_error("RAG callback busy");
        struct Busy {std::atomic<bool>&value;explicit Busy(std::atomic<bool>&v):value(v){value.store(true);}~Busy(){value.store(false);}} mark(*busy);
        json::Document ctx;ctx.SetObject();libai_invocation invocation;
        if(c){libai::rag_detail::scope(*c);field(ctx,"access_scope",c->access_scope,ctx.GetAllocator());invocation.cancellation=c->control.cancellation;invocation.deadline=c->control.deadline;invocation.idempotency_key=c->control.idempotency_key;}
        auto args=wire(arguments),scope=wire(ctx);libai_rag_result out;fn(user,op,args.c_str(),scope.c_str(),&out,c?&invocation:nullptr);
        if(c)libai::rag_detail::stopped(*c);
        if(out.failed||!out.error.empty()||!out.set)throw std::runtime_error(out.error.empty()?"RAG callback result missing or invalid":out.error);
        return parse(out.wire);
    }
};
struct Loader:libai::document_loader_t {
    std::shared_ptr<Callback> cb;explicit Loader(std::shared_ptr<Callback> c):cb(c){}
    std::vector<libai::document_t> load(const std::string &source,const libai::retrieval_context_t &ctx)override{json::Document d;d.SetObject();field(d,"source",source,d.GetAllocator());auto v=documents(cb->call("load",d,&ctx));libai::validate_documents(v,cb->limits);return v;}
};
struct Splitter:libai::text_splitter_t {
    std::shared_ptr<Callback> cb;explicit Splitter(std::shared_ptr<Callback> c):cb(c){}
    std::vector<libai::document_chunk_t> split(const std::vector<libai::document_t> &docs,const libai::retrieval_context_t &ctx)override{libai::validate_documents(docs,cb->limits);auto d=documents_json(docs);auto v=chunks(cb->call("split",d,&ctx));if(v.size()>cb->limits.max_chunks)throw std::invalid_argument("RAG chunk count exceeded");std::size_t bytes=0;std::set<std::string> ids;for(const auto &c:v){bytes+=libai::rag_detail::chunk(c,cb->limits);if(bytes>cb->limits.max_total_bytes||!ids.insert(c.id).second)throw std::invalid_argument("RAG chunk identity/bytes invalid");}return v;}
};
struct Embedding:libai::embedding_model_t {
    std::shared_ptr<Callback> cb;explicit Embedding(std::shared_ptr<Callback> c):cb(c){}
    std::string space_id()const override{json::Document d;d.SetObject();return string(cb->call("space_id",d,nullptr),128);}
    std::vector<std::vector<double>> embed(const std::vector<std::string> &texts,const libai::retrieval_context_t &ctx)override{if(texts.size()>cb->limits.max_chunks)throw std::invalid_argument("RAG embedding count exceeded");for(const auto &t:texts)libai::rag_detail::utf8(t,std::max(cb->limits.max_chunk_bytes,cb->limits.max_query_bytes),false);json::Document d;d.CopyFrom(strings_json(texts,d.GetAllocator()),d.GetAllocator());auto out=cb->call("embed",d,&ctx);array(out,256);std::vector<std::vector<double>> v;for(const auto &x:out.GetArray()){v.push_back(vector(x));libai::rag_detail::vector(v.back(),cb->limits);}return v;}
};
struct Store:libai::vector_store_t {
    std::shared_ptr<Callback> cb;explicit Store(std::shared_ptr<Callback> c):cb(c){}
    void replace_documents(const std::vector<std::string> &docs,const std::vector<libai::vector_record_t> &records,const std::string &space,const libai::retrieval_context_t &ctx)override{libai::prepared_index_t b;b.documents=docs;b.records=records;b.embedding_space=space;auto d=batch_json(b);auto out=cb->call("replace_documents",d,&ctx);keys(out,{});}
    libai::retrieval_result_t search(const libai::vector_query_t &q,const libai::retrieval_context_t &ctx)override{json::Document d;d.SetObject();auto &a=d.GetAllocator();d.AddMember("request",request_json(q.request,a),a);field(d,"embedding_space",q.embedding_space,a);d.AddMember("vector",vector_json(q.vector,a),a);return result(cb->call("search",d,&ctx));}
};
// Keep configured store limits in composed retrievers too, not only outer calls.
struct LimitedStore:libai::vector_store_t {
    std::shared_ptr<libai::vector_store_t> value;libai::rag_limits_t limits;
    LimitedStore(std::shared_ptr<libai::vector_store_t> v,libai::rag_limits_t l):value(v),limits(l){}
    void replace_documents(const std::vector<std::string>&d,const std::vector<libai::vector_record_t>&r,const std::string&s,const libai::retrieval_context_t&c)override{libai::validate_index_records(d,r,s,c,limits);value->replace_documents(d,r,s,c);libai::rag_detail::stopped(c);}
    libai::retrieval_result_t search(const libai::vector_query_t&q,const libai::retrieval_context_t&c)override{libai::rag_detail::request(q.request,c,limits);libai::rag_detail::utf8(q.embedding_space,128,false);libai::rag_detail::vector(q.vector,limits);auto r=value->search(q,c);libai::rag_detail::stopped(c);libai::rag_detail::hits(r,q.request,c,limits);return r;}
};
struct Retriever:libai::retriever_t {
    std::shared_ptr<Callback> cb;explicit Retriever(std::shared_ptr<Callback> c):cb(c){}
    libai::retrieval_result_t search(const libai::retrieval_request_t &q,const libai::retrieval_context_t &ctx)override{json::Document d;d.CopyFrom(request_json(q,d.GetAllocator()),d.GetAllocator());libai::rag_detail::request(q,ctx,cb->limits);auto v=result(cb->call("retrieve",d,&ctx));libai::rag_detail::hits(v,q,ctx,cb->limits);return v;}
};
}
extern "C" {
libai_rag *libai_rag_create(const char *kind,libai_rag_fn fn,void *user,libai_rag_release_fn release,const char *configuration){
    try{if(!fn)throw std::invalid_argument("RAG callback null");auto d=rag_bridge::parse(configuration?bridge_text(configuration):"{}");rag_bridge::keys(d,{"limits"});std::unique_ptr<libai_rag> h(new libai_rag);h->limits=rag_bridge::limits_field(d);auto cb=std::make_shared<Callback>(fn,user,release);cb->limits=h->limits;h->callback_busy=cb->busy;auto k=bridge_text(kind);
        if(k=="loader")h->loader=std::make_shared<Loader>(cb);else if(k=="splitter")h->splitter=std::make_shared<Splitter>(cb);else if(k=="embedding")h->embedding=std::make_shared<Embedding>(cb);else if(k=="store")h->store=std::make_shared<LimitedStore>(std::make_shared<Store>(cb),h->limits);else if(k=="retriever")h->retriever=std::make_shared<Retriever>(cb);else throw std::invalid_argument("RAG component kind unknown");cb->active=true;return h.release();
    }catch(const std::exception &e){bridge_error(e.what());}catch(...){bridge_error("RAG creation failed");}return nullptr;
}
libai_rag *libai_rag_create_splitter(const char *configuration){
    try{auto d=rag_bridge::parse(configuration?bridge_text(configuration):"{}");rag_bridge::keys(d,{"bytes","overlap","limits"});std::unique_ptr<libai_rag>h(new libai_rag);h->limits=rag_bridge::limits_field(d);h->splitter=std::make_shared<libai::utf8_text_splitter_t>(rag_bridge::number(d,"bytes",1024,65536),rag_bridge::number(d,"overlap",128,65536),h->limits);return h.release();}
    catch(const std::exception &e){bridge_error(e.what());}catch(...){bridge_error("RAG splitter creation failed");}return nullptr;
}
libai_rag *libai_rag_create_memory_store(const char *configuration){
    try{auto d=rag_bridge::parse(configuration?bridge_text(configuration):"{}");rag_bridge::keys(d,{"max_records","max_bytes","limits"});std::unique_ptr<libai_rag>h(new libai_rag);h->limits=rag_bridge::limits_field(d);h->store=std::make_shared<LimitedStore>(std::make_shared<libai::memory_vector_store_t>(rag_bridge::number(d,"max_records",4096,4096),rag_bridge::number(d,"max_bytes",16*1024*1024,64*1024*1024)),h->limits);return h.release();}
    catch(const std::exception &e){bridge_error(e.what());}catch(...){bridge_error("RAG store creation failed");}return nullptr;
}
libai_rag *libai_rag_create_vector_retriever(libai_rag *embedding,libai_rag *store,const char *configuration){
    try{auto eg=lock(embedding);auto sg=lock(store);if(!embedding->embedding||!store->store)throw std::invalid_argument("RAG embedding/store kinds required");auto d=rag_bridge::parse(configuration?bridge_text(configuration):"{}");rag_bridge::keys(d,{"limits"});std::unique_ptr<libai_rag>h(new libai_rag);h->limits=rag_bridge::limits_field(d);h->retriever=std::make_shared<libai::vector_retriever_t>(embedding->embedding,store->store,h->limits);return h.release();}
    catch(const std::exception &e){bridge_error(e.what());}catch(...){bridge_error("RAG vector retriever creation failed");}return nullptr;
}
int libai_rag_result_set(libai_rag_result *result,const char *wire,size_t bytes){
    if(!result)return -1;try{if(result->set||!wire||bytes>rag_bridge::wire_bytes)throw std::invalid_argument("RAG result missing, duplicate or oversized");result->wire.assign(wire,bytes);result->set=true;return 0;}
    catch(const std::exception &e){bridge_error(e.what());}catch(...){bridge_error("RAG result failed");}result->failed=true;return -1;
}
void libai_rag_result_error(libai_rag_result *result,const char *error){if(!result)return;try{if(!error||std::strlen(error)>4096||!error[0])result->failed=true;else result->error=error;}catch(...){result->failed=true;}}
char *libai_rag_load(libai_rag *handle,const char *source,const char *ctx,const libai_cancellation *token){
    try{auto g=lock(handle);if(!handle->loader)throw std::invalid_argument("RAG loader required");auto c=context(ctx,token);return bridge_owned(rag_bridge::wire(rag_bridge::documents_json(libai::load_documents(*handle->loader,bridge_text(source),c,handle->limits))));}
    catch(const std::exception &e){bridge_error(e.what());}catch(...){bridge_error("RAG load failed");}return nullptr;
}
char *libai_rag_prepare_index(libai_rag *splitter,libai_rag *embedding,const char *docs,const char *ctx,const char *limits,const libai_cancellation *token){
    try{auto sg=lock(splitter);auto eg=lock(embedding);if(!splitter->splitter||!embedding->embedding)throw std::invalid_argument("RAG splitter/embedding required");auto d=rag_bridge::parse(bridge_text(docs));auto l=rag_bridge::limits(rag_bridge::parse(limits?bridge_text(limits):"{}"));auto c=context(ctx,token);return bridge_owned(rag_bridge::wire(rag_bridge::batch_json(libai::prepare_index(rag_bridge::documents(d),*splitter->splitter,*embedding->embedding,c,l))));}
    catch(const std::exception &e){bridge_error(e.what());}catch(...){bridge_error("RAG prepare failed");}return nullptr;
}
int libai_rag_commit_index(libai_rag *store,const char *batch,const char *ctx,const libai_cancellation *token){
    try{auto g=lock(store);if(!store->store)throw std::invalid_argument("RAG store required");auto d=rag_bridge::parse(bridge_text(batch));auto c=context(ctx,token);libai::commit_index(*store->store,rag_bridge::batch(d),c,store->limits);return 0;}
    catch(const std::exception &e){bridge_error(e.what());}catch(...){bridge_error("RAG commit failed");}return -1;
}
char *libai_rag_retrieve(libai_rag *retriever,const char *query,const char *ctx,const libai_cancellation *token){
    try{auto g=lock(retriever);if(!retriever->retriever)throw std::invalid_argument("RAG retriever required");auto d=rag_bridge::parse(bridge_text(query));auto c=context(ctx,token);return bridge_owned(rag_bridge::wire(rag_bridge::result_json(libai::retrieve(*retriever->retriever,rag_bridge::request(d),c,retriever->limits))));}
    catch(const std::exception &e){bridge_error(e.what());}catch(...){bridge_error("RAG retrieve failed");}return nullptr;
}
int libai_rag_destroy(libai_rag *handle){if(!handle)return 0;try{auto g=lock(handle);g.unlock();delete handle;return 0;}catch(const std::exception &e){bridge_error(e.what());}catch(...){bridge_error("RAG destruction failed");}return -1;}
int libai_agent_add_retrieval_tool(libai_agent *agent,libai_rag *retriever,const char *name,const char *policy){
    try{if(!agent)throw std::invalid_argument("agent null");std::unique_lock<std::mutex>ag(agent->mutex,std::try_to_lock);if(!ag.owns_lock())throw std::logic_error("agent busy");auto g=lock(retriever);if(!retriever->retriever)throw std::invalid_argument("RAG retriever required");auto d=rag_bridge::parse(bridge_text(policy));rag_bridge::keys(d,{"execution_revision","access_scope","filter_json","requires_approval","side_effect_free","max_top_k","max_result_bytes","limits"});libai::rag_binding_t b;
        b.execution_revision=rag_bridge::text(d,"execution_revision",120);b.access_scope=rag_bridge::text(d,"access_scope",128);b.filter_json=rag_bridge::text(d,"filter_json",16384,"{}");b.requires_approval=rag_bridge::boolean(d,"requires_approval",true);b.side_effect_free=rag_bridge::boolean(d,"side_effect_free",false);b.max_top_k=rag_bridge::number(d,"max_top_k",8,64);b.max_result_bytes=rag_bridge::number(d,"max_result_bytes",32768,65536);b.limits=rag_bridge::limits_field(d);
        libai::add_retrieval_tool(*agent->runtime,retriever->retriever,bridge_text(name),b);return 0;
    }catch(const std::exception &e){bridge_error(e.what());}catch(...){bridge_error("RAG binding failed");}return -1;
}
}
