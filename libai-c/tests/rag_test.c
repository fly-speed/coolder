#include <libai.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *ctx="{\"access_scope\":\"tenant_a\",\"idempotency_key\":\"host-key\",\"timeout_ms\":5000}";
static const char *docs="[{\"id\":\"a\",\"source\":\"manual.txt\",\"revision\":\"r1\",\"text\":\"Alpha beta gamma\",\"metadata_json\":\"{\\\"kind\\\":\\\"manual\\\"}\"},{\"id\":\"b\",\"source\":\"other.txt\",\"revision\":\"r1\",\"text\":\"Other\"}]";
static const char *hit="{\"hits\":[{\"chunk\":{\"id\":\"a#0-5@r1\",\"document_id\":\"a\",\"source\":\"manual.txt\",\"revision\":\"r1\",\"text\":\"Alpha\",\"metadata_json\":\"{\\\"kind\\\":\\\"manual\\\"}\"},\"access_scope\":\"tenant_a\",\"score\":0.5}],\"truncated\":false}";
static const char *query="{\"query\":\"Alpha\",\"top_k\":2.0,\"filter_json\":\"{\\\"kind\\\":\\\"manual\\\"}\"}";
struct state {libai_rag *handle;int released,calls,embeds,spaces,commits,mode;libai_cancellation *token;char key[128];};
static void release(void *user){++((struct state*)user)->released;}
static void respond(libai_rag_result *r,const char *s){assert(libai_rag_result_set(r,s,strlen(s))==0);}
static void callback(void *user,const char *op,const char *args,const char *scope,libai_rag_result *r,libai_invocation *inv){
    struct state *s=user;++s->calls;
    if(strcmp(op,"space_id")==0){assert(!inv && strcmp(scope,"{}")==0);++s->spaces;respond(r,s->mode==3 && s->spaces%2==0?"\"changed\"":"\"embedding-v1\"");return;}
    assert(inv && !libai_invocation_stopped(inv));assert(strstr(scope,"access_scope"));
    assert(libai_invocation_remaining_ms(inv)>0);
    snprintf(s->key,sizeof(s->key),"%s",libai_invocation_idempotency_key(inv));
    if(s->handle){assert(libai_rag_destroy(s->handle)==-1);assert(!libai_rag_retrieve(s->handle,query,ctx,NULL));}
    if(s->mode==8){libai_rag_result_error(r,"backend failed after possible write");return;}
    if(s->mode==9)return;
    if(s->mode==10){respond(r,"{}");assert(libai_rag_result_set(r,"{}",2)==-1);return;}
    if(s->mode==11){assert(libai_rag_result_set(r,"x",128*1024*1024+1)==-1);return;}
    if(s->mode==14){respond(r,"{\"hits\":[],\"hits\":[]}");return;}
    if(s->mode==15){while(!libai_invocation_stopped(inv)){} respond(r,hit);return;}
    if(s->mode==12){libai_cancellation *saved=libai_invocation_retain_cancellation(inv);assert(saved);libai_cancellation_cancel(saved);assert(libai_invocation_stopped(inv));libai_cancellation_destroy(saved);respond(r,hit);return;}
    if(strcmp(op,"load")==0){assert(strstr(args,"manual.txt"));respond(r,docs);}
    else if(strcmp(op,"embed")==0){++s->embeds;assert(args[0]=='[');size_t count=0;const char *p=args;while((p=strchr(p,'"'))){++count;++p;}count/=2;
        if(s->mode==1)respond(r,"[[0,0]]");else if(s->mode==2)respond(r,"[]");else if(s->mode==4)respond(r,"[[NaN,1]]");else{
            size_t dimension=s->mode==13?16384:2;size_t capacity=count*(dimension*2+3)+3;char *out=malloc(capacity),*at=out;assert(out);*at++='[';for(size_t i=0;i<count;++i){if(i)*at++=',';*at++='[';for(size_t n=0;n<dimension;++n){if(n)*at++=',';*at++=s->mode==13 || n==0?'1':'0';}*at++=']';}*at++=']';*at=0;assert((size_t)(at-out)<capacity);respond(r,out);free(out);}
    }else if(strcmp(op,"split")==0){
        respond(r,"[{\"id\":\"x\",\"document_id\":\"a\",\"source\":\"manual.txt\",\"revision\":\"r1\",\"text\":\"wrong\",\"metadata_json\":\"{\\\"kind\\\":\\\"manual\\\"}\",\"has_span\":true,\"begin\":0,\"end\":5}]");
    }else if(strcmp(op,"replace_documents")==0){++s->commits;assert(strstr(args,"embedding-v1"));respond(r,"{}");}
    else if(strcmp(op,"retrieve")==0 || strcmp(op,"search")==0){assert(strstr(args,"Alpha"));if(s->mode==5){char out[1024];snprintf(out,sizeof(out),"%s",hit);char *p=strstr(out,"tenant_a");assert(p);memcpy(p,"tenant_b",8);respond(r,out);}else if(s->mode==6){char out[1024];snprintf(out,sizeof(out),"%s",hit);char *p=strstr(out,"\\\"manual\\\"");assert(p);memcpy(p+2,"public",6);respond(r,out);}else respond(r,hit);}
    else assert(0);
}
static libai_rag *component(const char *kind,struct state *s){libai_rag *r=libai_rag_create(kind,callback,s,release,NULL);assert(r);s->handle=r;return r;}
static void destroy(libai_rag *r,struct state *s){s->handle=NULL;assert(libai_rag_destroy(r)==0);}
struct model_state {int calls,authorized,mode;};
static void model(void *user,const char *request,libai_response *out,libai_invocation *inv){
    struct model_state *s=user;(void)inv;if(++s->calls==1){assert(strstr(request,"knowledge_search"));assert(strstr(request,"requires_approval"));libai_response_call(out,"r1","knowledge_search",s->mode==1?"{\"query\":\"Alpha\",\"access_scope\":\"tenant_b\"}":s->mode==2?"{\"query\":\"Alpha\",\"top_k\":1.5}":"{\"query\":\"Alpha\",\"top_k\":2.0}");}else{if(s->mode==0)assert(strstr(request,"citation") && strstr(request,"manual.txt"));libai_response_text(out,"done","");}}
static int authorize(void *user,const char *name,const char *arguments,int approval){struct model_state *s=user;(void)arguments;assert(approval && strcmp(name,"knowledge_search")==0);++s->authorized;return 1;}
static const char *policy="{\"execution_revision\":\"corpus-acl-embedding-v1\",\"access_scope\":\"tenant_a\",\"max_top_k\":2,\"filter_json\":\"{\\\"kind\\\":\\\"manual\\\"}\"}";
struct journal {char *revision,*state;int sequence,leased;};
static char *copy(const char *s){size_t n=strlen(s)+1;char *p=malloc(n);assert(p);memcpy(p,s,n);return p;}
static void storage(void *user,const char *op,const char *scope,const char *revision,const char *state,libai_execution_result *out){struct journal *s=user;(void)scope;
    if(strcmp(op,"open")==0){assert(!s->leased);s->leased=1;libai_execution_result_set(out,s->revision?s->revision:"",s->state?s->state:"","");}
    else if(strcmp(op,"commit")==0){assert(s->leased && strcmp(revision,s->revision?s->revision:"")==0);char next[24];snprintf(next,sizeof(next),"%d",++s->sequence);free(s->revision);free(s->state);s->revision=copy(next);s->state=copy(state);libai_execution_result_set(out,s->revision,"","");}
    else{assert(strcmp(op,"close")==0);s->leased=0;}}
static void large_model(void *user,const char *request,libai_response *out,libai_invocation *inv){(void)request;(void)inv;libai_response_text(out,(const char*)user,"");}
int main(void){
    struct state loader={0},embedding={0};assert(!libai_rag_create("unknown",callback,&loader,release,NULL) && loader.released==0);
    assert(!libai_rag_create("loader",NULL,&loader,release,NULL));
    assert(!libai_rag_create_splitter("{\"bytes\":4,\"overlap\":4}"));assert(!libai_rag_create_memory_store("{\"max_records\":0}"));
    libai_rag *l=component("loader",&loader),*e=component("embedding",&embedding),*split=libai_rag_create_splitter("{\"bytes\":64,\"overlap\":0}"),*store=libai_rag_create_memory_store(NULL);assert(split && store);
    char *loaded=libai_rag_load(l,"manual.txt",ctx,NULL);assert(loaded && strstr(loaded,"Alpha beta"));libai_string_free(loaded);
    assert(!libai_rag_load(l,"manual.txt","{\"access_scope\":\"../bad\"}",NULL) && loader.calls==1);
    char *prepared=libai_rag_prepare_index(split,e,docs,ctx,NULL,NULL);assert(prepared && strstr(prepared,"records") && embedding.embeds==1);assert(strcmp(embedding.key,"host-key")==0);
    libai_rag *v=libai_rag_create_vector_retriever(e,store,NULL);assert(v);
    char *found=libai_rag_retrieve(v,query,ctx,NULL);assert(found && !strstr(found,"Alpha beta"));libai_string_free(found);
    assert(libai_rag_commit_index(store,prepared,ctx,NULL)==0);found=libai_rag_retrieve(v,query,ctx,NULL);assert(found && strstr(found,"Alpha beta") && !strstr(found,"Other"));libai_string_free(found);
    found=libai_rag_retrieve(v,"{\"query\":\"Alpha\",\"top_k\":1}",ctx,NULL);assert(found && strstr(found,"\"truncated\":true"));libai_string_free(found);
    found=libai_rag_retrieve(v,query,"{\"access_scope\":\"tenant_b\"}",NULL);assert(found && !strstr(found,"Alpha beta"));libai_string_free(found);
    assert(!libai_rag_prepare_index(split,e,docs,ctx,"{\"max_chunks\":1}",NULL));
    for(int mode=1;mode<=4;++mode){embedding.mode=mode;embedding.spaces=0;assert(!libai_rag_prepare_index(split,e,docs,ctx,NULL,NULL));}embedding.mode=0;
    struct state bad_split={0};libai_rag *bs=component("splitter",&bad_split);int embeds=embedding.embeds;assert(!libai_rag_prepare_index(bs,e,docs,ctx,NULL,NULL) && embedding.embeds==embeds);destroy(bs,&bad_split);assert(bad_split.released==1);
    assert(libai_rag_commit_index(store,"{\"documents\":[\"a\"],\"embedding_space\":\"embedding-v1\",\"records\":[]}",ctx,NULL)==0);
    found=libai_rag_retrieve(v,query,ctx,NULL);assert(found && !strstr(found,"Alpha beta"));libai_string_free(found);
    assert(libai_rag_commit_index(store,prepared,ctx,NULL)==0);
    libai_cancellation *cancel=libai_cancellation_create();libai_cancellation_cancel(cancel);embeds=embedding.embeds;assert(!libai_rag_retrieve(v,query,ctx,cancel) && embeds==embedding.embeds);libai_cancellation_destroy(cancel);
    /* Composed retriever retains embedding/store after outer handles disappear. */
    destroy(e,&embedding);assert(embedding.released==0);assert(libai_rag_destroy(store)==0);found=libai_rag_retrieve(v,query,ctx,NULL);assert(found && strstr(found,"Alpha beta"));libai_string_free(found);
    struct model_state m={0};libai_agent *agent=libai_agent_create(model,&m,"");assert(agent);assert(libai_agent_add_retrieval_tool(agent,v,"knowledge_search",policy)==0);assert(libai_rag_destroy(v)==0 && embedding.released==0);
    libai_hooks hooks={0};hooks.authorize=authorize;hooks.user=&m;struct journal j={0};assert(libai_agent_set_execution_store(agent,"rag-cache",storage,&j)==0);
    char *run=libai_agent_run(agent,"search",NULL,&hooks);assert(run && strstr(run,"completed") && strstr(run,"\"accepted\":false") && m.calls==2 && m.authorized==1);libai_string_free(run);embeds=embedding.embeds;
    run=libai_agent_run(agent,"search",NULL,&hooks);assert(run && m.calls==2 && embedding.embeds==embeds);libai_string_free(run);assert(libai_agent_destroy(agent)==0 && embedding.released==1);free(j.state);free(j.revision);
    libai_string_free(prepared);assert(libai_rag_destroy(split)==0);destroy(l,&loader);assert(loader.released==1);
    /* Direct retriever enforces access/filter before returning results. */
    for(int mode=0;mode<=12;++mode){if(mode>=1&&mode<=4)continue;struct state r={0};libai_rag *h=component("retriever",&r);r.mode=mode;found=libai_rag_retrieve(h,query,ctx,NULL);if(mode==0||mode==7){assert(found && strstr(found,"Alpha"));libai_string_free(found);}else assert(!found);destroy(h,&r);assert(r.released==1);}
    for(int mode=1;mode<=3;++mode){struct state r={0};libai_rag *h=component("retriever",&r);struct model_state denied={0};denied.mode=mode;agent=libai_agent_create(model,&denied,"");assert(agent && libai_agent_add_retrieval_tool(agent,h,"knowledge_search",policy)==0);hooks.user=&denied;
        run=libai_agent_run(agent,"search",NULL,mode==3?NULL:&hooks);assert(run && r.calls==0 && denied.authorized==0);libai_string_free(run);assert(libai_agent_destroy(agent)==0);destroy(h,&r);}
    /* External vector store is called only on explicit commit; response errors
       are not retried and cannot prove a write did not occur. */
    struct state ext={0},emb={0};libai_rag *es=component("store",&ext);e=component("embedding",&emb);split=libai_rag_create_splitter("{\"bytes\":64,\"overlap\":0}");prepared=libai_rag_prepare_index(split,e,docs,ctx,NULL,NULL);assert(prepared && ext.calls==0);
    assert(libai_rag_commit_index(es,prepared,ctx,NULL)==0 && ext.commits==1);v=libai_rag_create_vector_retriever(e,es,NULL);assert(v);found=libai_rag_retrieve(v,query,ctx,NULL);assert(found && strstr(found,"Alpha"));libai_string_free(found);
    ext.mode=8;int calls=ext.calls;assert(libai_rag_commit_index(es,prepared,ctx,NULL)==-1 && ext.calls==calls+1);ext.mode=0;
    assert(libai_rag_commit_index(es,"{\"documents\":[\"a\"],\"records\":[],\"embedding_space\":\"embedding-v1\",\"extra\":1}",ctx,NULL)==-1 && ext.calls==calls+1);
    libai_string_free(prepared);destroy(e,&emb);destroy(es,&ext);assert(!emb.released && !ext.released);assert(libai_rag_destroy(v)==0 && emb.released==1 && ext.released==1);assert(libai_rag_destroy(split)==0);
    /* Vector batches can exceed the ordinary 65,536-node JSON budget without
       relaxing model/tool/metadata guards. Six vectors at the maximum dimension. */
    struct state wide={0};wide.mode=13;e=component("embedding",&wide);split=libai_rag_create_splitter("{\"bytes\":4,\"overlap\":0}");store=libai_rag_create_memory_store(NULL);assert(split && store);
    prepared=libai_rag_prepare_index(split,e,docs,ctx,NULL,NULL);assert(prepared && strlen(prepared)>180000);assert(libai_rag_commit_index(store,prepared,ctx,NULL)==0);v=libai_rag_create_vector_retriever(e,store,NULL);assert(v);found=libai_rag_retrieve(v,query,ctx,NULL);assert(found && strstr(found,"tenant_a"));libai_string_free(found);libai_string_free(prepared);
    wide.mode=0;prepared=libai_rag_prepare_index(split,e,"[{\"id\":\"unicode\",\"source\":\"utf8.txt\",\"revision\":\"r1\",\"text\":\"中文a\"}]",ctx,NULL,NULL);assert(prepared && strstr(prepared,"\"begin\":3,\"end\":7"));libai_string_free(prepared);
    assert(!libai_rag_prepare_index(split,e,"[{\"id\":\"a\",\"id\":\"b\"}]",ctx,NULL,NULL));
    destroy(e,&wide);assert(libai_rag_destroy(store)==0 && libai_rag_destroy(v)==0 && wide.released==1 && libai_rag_destroy(split)==0);
    for(int mode=14;mode<=15;++mode){struct state r={0};r.mode=mode;libai_rag *h=component("retriever",&r);assert(!libai_rag_retrieve(h,query,mode==15?"{\"access_scope\":\"tenant_a\",\"timeout_ms\":20}":ctx,NULL));destroy(h,&r);}
    /* The larger RAG envelope must not relax final model JSON validation. */
    size_t values=65536;char *large=malloc(values*2+2);assert(large);char *at=large;*at++='[';for(size_t i=0;i<values;++i){if(i)*at++=',';*at++='1';}*at++=']';*at=0;
    agent=libai_agent_create(large_model,large,"");assert(agent && libai_agent_configure_output(agent,"{\"type\":\"array\"}",0)==0);libai_options options;libai_options_default(&options);options.max_context_bytes=1024*1024;
    run=libai_agent_run(agent,"check",&options,NULL);assert(run && strstr(run,"invalid_json_or_resource_limit") && strstr(run,"\"status\":\"protocol_error\"") && strstr(run,"\"output_validated\":false"));libai_string_free(run);at[-3]=']';at[-2]=0;assert(libai_agent_reset(agent)==0);run=libai_agent_run(agent,"check",&options,NULL);assert(run && strstr(run,"\"status\":\"completed\"") && strstr(run,"\"output_validated\":true"));libai_string_free(run);assert(libai_agent_destroy(agent)==0);free(large);
    puts("C RAG indexing/retrieval/ownership/governance passed");return 0;
}
