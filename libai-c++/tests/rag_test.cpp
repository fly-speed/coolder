#include <libai_cpp/rag.h>
#include <libai_cpp/agent.h>
#include <cassert>
#include <limits>
using namespace libai::cpp;
template<class F>void rejects(F f){bool failed=false;try{f();}catch(...){failed=true;}assert(failed);}
Document document(std::string id,std::string text){Document d;d.id=id;d.source="memory://"+id;d.revision="v1";d.text=text;d.metadata_json=R"({"topic":"guide"})";return d;}
RetrievalContext context(){RetrievalContext c;c.access_scope="tenant";return c;}
struct Loader:DocumentLoader{std::vector<Document> load(const std::string&s,const RetrievalContext&c)override{assert(s=="fixture"&&c.access_scope=="tenant");return {document("restart","restart instructions"),document("install","install instructions")};}};
struct Embedding:EmbeddingModel {
    int calls=0;bool invalid=false;std::string space_id()const override{return "mock/2/v1";}
    std::vector<std::vector<double>> embed(const std::vector<std::string>&input,const RetrievalContext &c)override{assert(!c.stopped());++calls;std::vector<std::vector<double>> out;for(const auto &s:input)out.push_back(invalid?std::vector<double>{std::numeric_limits<double>::infinity(),0}:s.find("restart")!=std::string::npos?std::vector<double>{1,0}:std::vector<double>{0,1});return out;}
};
void independent_index_and_retrieval(){
    Loader loader;auto c=context();auto docs=Rag::load_documents(loader,"fixture",c);auto model=std::make_shared<Embedding>();auto store=std::make_shared<MemoryVectorStore>();Utf8TextSplitter splitter(1024,0);
    auto prepared=Rag::prepare_index(docs,splitter,*model,c);assert(prepared.records.size()==2&&model->calls==1);Rag::commit_index(*store,prepared,c);
    VectorRetriever retriever(model,store);RetrievalRequest q;q.query="restart";q.top_k=2;q.filter_json=R"({"topic":"guide"})";auto hits=Rag::retrieve(retriever,q,c);assert(hits.hits.size()==2&&hits.hits[0].chunk.document_id=="restart"&&hits.hits[0].score==1);
    auto other=c;other.access_scope="other";assert(Rag::retrieve(retriever,q,other).hits.empty());
    docs[0].text="install changed";docs[0].revision="v2";prepared=Rag::prepare_index({docs[0]},splitter,*model,c);Rag::commit_index(*store,prepared,c);hits=Rag::retrieve(retriever,q,c);for(const auto &hit:hits.hits)if(hit.chunk.document_id=="restart")assert(hit.chunk.revision=="v2");
    model->invalid=true;rejects([&]{Rag::prepare_index(docs,splitter,*model,c);});model->invalid=false;
    auto invalid=prepared;invalid.records[0].chunk.document_id="outside";rejects([&]{Rag::commit_index(*store,invalid,c);});assert(Rag::retrieve(retriever,q,c).hits.size()==2);
    docs[0].text.clear();prepared=Rag::prepare_index({docs[0]},splitter,*model,c);Rag::commit_index(*store,prepared,c);assert(Rag::retrieve(retriever,q,c).hits.size()==1);
    auto text=document("unicode","abc🌏你好def");Utf8TextSplitter small(7,2);auto chunks=small.split({text},c);for(const auto &chunk:chunks)assert(chunk.has_span&&text.text.substr(chunk.begin,chunk.end-chunk.begin)==chunk.text&&chunk.text.size()<=7);
    text.text=std::string(2*1024*1024+1,'x');rejects([&]{Rag::validate_documents({text});});
    c.control.cancellation=std::make_shared<Cancellation>();c.control.cancellation->cancel();rejects([&]{Rag::retrieve(retriever,q,c);});
}
struct Store:ExecutionStore {ExecutionSnapshot value;int revision=0;bool leased=false;
    ExecutionSnapshot open(const std::string&)override{assert(!leased);leased=true;return value;}
    std::string commit(const std::string&,const std::string&old,const std::string&state)override{assert(leased&&old==value.revision);value.state_json=state;return value.revision=std::to_string(++revision);}
    void close(const std::string&)noexcept override{leased=false;}
};
void agent_scope_cancellation_and_ownership(){
    int calls=0,models=0,authorizations=0;std::shared_ptr<Cancellation> retained;
    auto retriever=std::make_shared<FunctionRetriever>([&](const RetrievalRequest&q,const RetrievalContext&c){++calls;assert(q.top_k==2&&q.filter_json==R"({"topic":"guide"})"&&c.access_scope=="tenant"&&c.control.idempotency_key=="job.0");retained=c.control.cancellation;assert(retained&&!retained->cancelled());RetrievalHit h;h.access_scope="tenant";h.chunk.id="cite";h.chunk.document_id="guide";h.chunk.source="memory://guide";h.chunk.revision="v1";h.chunk.text="restart";h.chunk.metadata_json=q.filter_json;h.score=1;RetrievalResult r;r.hits={h};return r;});
    std::weak_ptr<Retriever> weak=retriever;
    Agent agent([&](const ModelRequest &q,const StreamSink&){ModelResponse r;if(++models==1){ToolCall c;c.id="one";c.name="search";c.arguments=R"({"query":"restart","top_k":2.0})";r.calls={c};}else{assert(q.messages.back().text.find("citation")!=std::string::npos);r.text="answer";}return r;});
    RagBinding binding;binding.access_scope="tenant";binding.execution_revision="corpus1-acl1";binding.filter_json=R"({"topic":"guide"})";agent.add_retrieval_tool(retriever,"search",binding);retriever.reset();assert(!weak.expired());
    RunOptions options;options.execution_store=std::make_shared<Store>();options.execution_scope="job";options.cancellation=std::make_shared<Cancellation>();options.authorize=[&](const ToolSpec &s,const ToolCall&){++authorizations;return s.requires_approval;};
    auto result=agent.run("help",options);assert(result.status==RunStatus::completed&&calls==1&&models==2&&authorizations==1&&!result.task_contract.accepted());
    assert(agent.run("help",options).status==RunStatus::completed&&calls==1&&models==2);options.cancellation->cancel();assert(retained->cancelled());
    int unauthorized=0;auto blocked=std::make_shared<FunctionRetriever>([&](const RetrievalRequest&,const RetrievalContext&){++unauthorized;return RetrievalResult();});int turns=0;
    Agent denied([&](const ModelRequest&,const StreamSink&){ModelResponse r;if(++turns==1){ToolCall c;c.id="one";c.name="search";c.arguments=R"({"query":"x"})";r.calls={c};}else r.text="done";return r;});denied.add_retrieval_tool(blocked,"search",binding);assert(denied.run("help").status==RunStatus::completed&&unauthorized==0);
    FunctionRetriever wrong([](const RetrievalRequest&,const RetrievalContext&){RetrievalResult r;RetrievalHit h;h.access_scope="outside";h.chunk.id="id";h.chunk.document_id="doc";h.chunk.source="source";h.chunk.revision="v1";h.chunk.text="data";r.hits={h};return r;});RetrievalRequest q;q.query="x";rejects([&]{Rag::retrieve(wrong,q,context());});
}
int main(){independent_index_and_retrieval();agent_scope_cancellation_and_ownership();}
