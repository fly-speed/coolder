#include <libai/rag.h>
#include <cassert>
#include <limits>
using namespace libai;
template<class F>void rejects(F f){bool rejected=false;try{f();}catch(...){rejected=true;}assert(rejected);}
document_t doc(std::string id,std::string text,std::string topic="guide"){document_t d;d.id=id;d.source="memory://"+id;d.revision="v1";d.text=text;d.metadata_json="{\"topic\":\""+topic+"\"}";return d;}
retrieval_context_t context(std::string scope="tenant"){retrieval_context_t c;c.access_scope=scope;return c;}
struct Embedding:embedding_model_t {
    std::string space="mock/two-dim/v1";int calls=0;bool invalid=false,change=false;
    std::string space_id()const override{return space;}
    std::vector<std::vector<double>> embed(const std::vector<std::string>&texts,const retrieval_context_t &c)override{
        assert(!c.stopped());++calls;std::vector<std::vector<double>> result;
        for(const auto &text:texts)result.push_back(invalid?std::vector<double>{std::numeric_limits<double>::infinity(),0}:text.find("restart")!=std::string::npos?std::vector<double>{1,0}:std::vector<double>{0,1});
        if(change)space="changed";return result;
    }
};
struct Loader:document_loader_t {
    int calls=0;
    std::vector<document_t> load(const std::string &source,const retrieval_context_t &c)override{++calls;assert(source=="fixture"&&c.access_scope=="tenant");return {doc("restart","restart instructions"),doc("install","install instructions"),doc("private","restart private","secret")};}
};
void index_query_isolation_and_atomicity(){
    Loader loader;auto c=context();auto docs=load_documents(loader,"fixture",c);assert(loader.calls==1);
    utf8_text_splitter_t splitter(1024,0);auto embedding=std::make_shared<Embedding>();auto store=std::make_shared<memory_vector_store_t>();
    auto batch=prepare_index(docs,splitter,*embedding,c);assert(batch.records.size()==3&&embedding->calls==1);commit_index(*store,batch,c);
    vector_retriever_t retriever(embedding,store);retrieval_request_t q;q.query="restart";q.top_k=2;q.filter_json=R"({"topic":"guide"})";
    auto result=retrieve(retriever,q,c);assert(result.hits.size()==2&&result.hits[0].chunk.document_id=="restart"&&result.hits[0].score==1&&result.hits[0].chunk.revision=="v1");
    assert(retrieve(retriever,q,context("other")).hits.empty());
    docs[0].revision="v2";docs[0].text="install changed";auto replace=prepare_index({docs[0]},splitter,*embedding,c);commit_index(*store,replace,c);result=retrieve(retriever,q,c);
    for(const auto &h:result.hits)if(h.chunk.document_id=="restart")assert(h.chunk.revision=="v2");
    auto invalid=replace;invalid.records[0].chunk.document_id="outside";rejects([&]{commit_index(*store,invalid,c);});assert(retrieve(retriever,q,c).hits.size()==2);
    document_t empty=docs[0];empty.text.clear();auto removed=prepare_index({empty},splitter,*embedding,c);assert(removed.records.empty());commit_index(*store,removed,c);assert(retrieve(retriever,q,c).hits.size()==1);
    auto old_space=embedding->space;embedding->space="wrong";rejects([&]{retrieve(retriever,q,c);});embedding->space=old_space;
    embedding->invalid=true;rejects([&]{prepare_index(docs,splitter,*embedding,c);});embedding->invalid=false;embedding->change=true;rejects([&]{prepare_index(docs,splitter,*embedding,c);});
    memory_vector_store_t limited(1,4096);rejects([&]{commit_index(limited,batch,c);});vector_query_t v;v.request=q;v.embedding_space=old_space;v.vector={1,0};assert(limited.search(v,c).hits.empty());
    batch.records[0].vector={1e308,0};batch.records[1].vector={0,1e308};batch.records[2].vector={1e308,0};store->replace_documents(batch.documents,batch.records,old_space,c);v.vector={1e308,0};assert(store->search(v,c).hits[0].score==1);
}
void utf8_provenance_and_bounds(){
    auto c=context();auto d=doc("unicode","abc🌏你好def");utf8_text_splitter_t splitter(7,2);auto chunks=splitter.split({d},c);assert(chunks.size()>1);
    for(const auto &h:chunks){assert(h.has_span&&h.text==d.text.substr(h.begin,h.end-h.begin)&&h.text.size()<=7);rag_detail::chunk(h,rag_limits_t());}
    rejects([&]{utf8_text_splitter_t invalid(3,1);});d.text=std::string("\xc0\xaf",2);rejects([&]{splitter.split({d},c);});d=doc("d","valid");d.metadata_json=R"({"a":1,"a":2})";rejects([&]{validate_documents({d});});d=doc("d","valid");rejects([&]{validate_documents({d,d});});
    d.text=std::string(2*1024*1024+1,'x');rejects([&]{validate_documents({d});});
    struct Tamper:text_splitter_t{std::vector<document_chunk_t> split(const std::vector<document_t>&docs,const retrieval_context_t &c)override{utf8_text_splitter_t s(1024,0);auto chunks=s.split(docs,c);chunks[0].source="wrong";return chunks;}} tamper;
    Embedding embedding;rejects([&]{prepare_index({doc("d","valid")},tamper,embedding,c);});assert(embedding.calls==0);
    int calls=0;function_retriever_t backend([&](const retrieval_request_t &,const retrieval_context_t &ctx){++calls;retrieval_result_t r;retrieval_hit_t h;h.access_scope=ctx.access_scope;h.chunk.id="citation";h.chunk.document_id="d";h.chunk.source="memory://d";h.chunk.revision="v1";h.chunk.text="text";r.hits={h};return r;});
    retrieval_request_t q;q.query="query";rejects([&]{retrieve(backend,q,context(""));});q.top_k=0;rejects([&]{retrieve(backend,q,c);});q.top_k=8;q.query=std::string(8193,'x');rejects([&]{retrieve(backend,q,c);});assert(calls==0);
    q.query="query";c.control.cancellation=std::make_shared<cancellation_t>();c.control.cancellation->cancel();rejects([&]{retrieve(backend,q,c);});assert(calls==0);
}
void backend_result_validation_and_budget(){
    retrieval_hit_t hit;hit.access_scope="tenant";hit.chunk.id="cite";hit.chunk.document_id="doc";hit.chunk.source="memory://doc";hit.chunk.revision="v1";hit.chunk.text="restart";hit.chunk.metadata_json=R"({"topic":"guide"})";
    retrieval_result_t value;value.hits={hit};function_retriever_t backend([&](const retrieval_request_t &,const retrieval_context_t &){return value;});retrieval_request_t q;q.query="restart";q.filter_json=R"({"topic":"guide"})";
    assert(retrieve(backend,q,context()).hits.size()==1);
    value.hits[0].access_scope="other";rejects([&]{retrieve(backend,q,context());});value.hits={hit,hit};rejects([&]{retrieve(backend,q,context());});
    value.hits={hit};value.hits[0].score=std::numeric_limits<double>::quiet_NaN();rejects([&]{retrieve(backend,q,context());});value.hits[0]=hit;value.hits[0].chunk.metadata_json=R"({"topic":"secret"})";rejects([&]{retrieve(backend,q,context());});
    value.hits={hit};value.hits[0].chunk.text=std::string(5000,'x');auto wire=rag_detail::result_json(value,256);rag_detail::json::Document d;governance_detail::parse_json(wire,d);assert(d["truncated"].GetBool()&&d["hits"].Empty()&&wire.size()<=256);
}
struct Store:execution_store_t {
    execution_snapshot_t value;bool active=false;int revision=0;
    execution_snapshot_t open(const std::string &)override{assert(!active);active=true;return value;}
    std::string commit(const std::string &,const std::string &expected,const std::string &state)override{assert(active&&expected==value.revision);value.state_json=state;return value.revision=std::to_string(++revision);}
    void close(const std::string &)noexcept override{active=false;}
};
void agent_governance_citations_and_durable(){
    int calls=0,models=0,authorizations=0;auto retriever=std::make_shared<function_retriever_t>([&](const retrieval_request_t &q,const retrieval_context_t &c){++calls;assert(q.filter_json==R"({"topic":"guide"})"&&c.access_scope=="tenant"&&c.control.idempotency_key=="task.0");retrieval_result_t r;retrieval_hit_t h;h.access_scope="tenant";h.chunk.id="guide#0-7@v1";h.chunk.document_id="guide";h.chunk.source="memory://guide";h.chunk.revision="v1";h.chunk.text="restart";h.chunk.metadata_json=q.filter_json;h.chunk.has_span=true;h.chunk.end=7;h.score=1;r.hits={h};return r;});
    auto model=std::make_shared<function_model_t>([&](const model_request_t &request,const stream_sink_t &){model_response_t r;if(++models==1){tool_call_t c;c.id="one";c.name="knowledge.search";c.arguments=R"({"query":"restart","top_k":2.0})";r.calls={c};}else{assert(request.messages.back().text.find("guide#0-7@v1")!=std::string::npos);r.text="Use the cited restart passage.";}return r;});
    rag_binding_t binding;binding.execution_revision="corpus-v1-acl-v1";binding.access_scope="tenant";binding.filter_json=R"({"topic":"guide"})";
    agent_t agent(model);add_retrieval_tool(agent,retriever,"knowledge.search",binding);agent_session_t session;run_options_t options;options.execution_store=std::make_shared<Store>();options.execution_scope="task";
    options.authorize=[&](const tool_spec_t &spec,const tool_call_t &){++authorizations;return spec.requires_approval;};auto r=agent.run(session,"help",options);assert(r.status==run_status_t::completed&&models==2&&calls==1&&authorizations==1&&!r.task_contract.accepted());
    agent_session_t resumed;assert(agent.run(resumed,"help",options).status==run_status_t::completed&&models==2&&calls==1);
    int unsafe_calls=0;auto unsafe=std::make_shared<function_retriever_t>([&](const retrieval_request_t &,const retrieval_context_t &){++unsafe_calls;return retrieval_result_t();});
    for(int scenario=0;scenario<3;++scenario){int turn=0;agent_t rejected(std::make_shared<function_model_t>([&](const model_request_t &,const stream_sink_t &){model_response_t r;if(++turn==1){tool_call_t c;c.id="a";c.name="search";c.arguments=scenario==2?R"({"query":"x","top_k":1.5})":scenario?R"({"query":"x","access_scope":"other"})":R"({"query":"x"})";r.calls={c};}else r.text="done";return r;}));add_retrieval_tool(rejected,unsafe,"search",binding);agent_session_t s;assert(rejected.run(s,"help").status==run_status_t::completed&&unsafe_calls==0);}
}
int main(){index_query_isolation_and_atomicity();utf8_provenance_and_bounds();backend_result_validation_and_budget();agent_governance_citations_and_durable();}
