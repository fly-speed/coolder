#include <libai.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct transport_state {
    libai_mcp *handle;
    int initialized,called,closed,released,lose,stop,missing,duplicate,oversize;
    int cancelled;
    libai_cancellation *token;
    char key[128];
};
static void release(void *user){++((struct transport_state*)user)->released;}
static void transport(void *user,const char *op,const char *wire,const char *context,
    libai_mcp_result *result,libai_invocation *invocation){
    struct transport_state *s=user;char response[2048];const char *value;
    if(strcmp(op,"close")==0){assert(!invocation);++s->closed;return;}
    assert(invocation && !libai_invocation_stopped(invocation));
    if(strcmp(op,"notify")==0){
        if(strstr(wire,"notifications/initialized")){assert(strstr(context,"2025-11-25"));++s->initialized;}
        else if(strstr(wire,"notifications/cancelled"))++s->cancelled;
        return;
    }
    assert(strcmp(op,"request")==0);
    if(s->handle){assert(!libai_mcp_call(s->handle,"list_tools","{}",NULL));assert(libai_mcp_destroy(s->handle)==-1);}
    if(strstr(wire,"\"method\":\"initialize\"")){
        value="{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"fixture\",\"version\":\"1\"}}";
    }else if(strstr(wire,"\"method\":\"tools/list\"")){
        assert(s->initialized);value="{\"tools\":[{\"name\":\"echo\",\"inputSchema\":{\"type\":\"object\"},\"annotations\":{\"readOnlyHint\":true}}]}";
    }else{
        ++s->called;snprintf(s->key,sizeof(s->key),"%s",libai_invocation_idempotency_key(invocation));
        if(s->lose){libai_mcp_result_error(result,"lost response after effect");return;}
        if(s->stop){libai_cancellation_cancel(s->token);assert(libai_invocation_stopped(invocation));libai_mcp_result_error(result,"transport cancelled");return;}
        value="{\"content\":[{\"type\":\"text\",\"text\":\"echo\"}],\"_meta\":{\"private\":\"secret\"}}";
    }
    if(s->missing)return;
    const char *id=strstr(context,"\"request_id\":");assert(id);
    snprintf(response,sizeof(response),"{\"jsonrpc\":\"2.0\",\"id\":%llu,\"result\":%s}",strtoull(id+13,NULL,10),value);
    if(s->oversize){assert(libai_mcp_result_response(result,response,2*1024*1024+1)==-1);return;}
    assert(libai_mcp_result_response(result,response,strlen(response))==0);
    if(s->duplicate)assert(libai_mcp_result_response(result,response,strlen(response))==-1);
}
static libai_mcp *connect(struct transport_state *s){
    libai_mcp *c=libai_mcp_create(transport,s,release);assert(c);s->handle=c;
    char *v=libai_mcp_call(c,"initialize","{\"name\":\"test\",\"version\":\"1\"}",NULL);assert(v);libai_string_free(v);
    v=libai_mcp_call(c,"list_tools","{}",NULL);assert(v && strstr(v,"input_schema"));libai_string_free(v);return c;
}
struct model_state {int calls,invalid,authorized;};
static void model(void *user,const char *request,libai_response *out,libai_invocation *invocation){
    struct model_state *s=user;(void)invocation;
    if(++s->calls==1){assert(strstr(request,"\"requires_approval\":true"));
        libai_response_call(out,"call1","local_echo",s->invalid?"{\"bad\":true}":"{}");
    }else libai_response_text(out,"done","");
}
static int authorize(void *user,const char *name,const char *arguments,int approval){
    struct model_state *s=user;(void)arguments;assert(approval && strcmp(name,"local_echo")==0);++s->authorized;return 1;
}
static const char *policy="{\"parameters_schema\":\"{\\\"type\\\":\\\"object\\\",\\\"additionalProperties\\\":false}\",\"execution_revision\":\"host-fixture-1\"}";
struct store {char *revision,*state;int sequence,leased;};
static char *copy(const char *s){size_t n=strlen(s)+1;char *p=malloc(n);assert(p);memcpy(p,s,n);return p;}
static void storage(void *user,const char *op,const char *scope,const char *revision,const char *state,libai_execution_result *out){
    struct store *s=user;(void)scope;
    if(strcmp(op,"open")==0){assert(!s->leased);s->leased=1;libai_execution_result_set(out,s->revision?s->revision:"",s->state?s->state:"","");}
    else if(strcmp(op,"commit")==0){assert(s->leased && strcmp(revision,s->revision?s->revision:"")==0);char next[24];snprintf(next,sizeof(next),"%d",++s->sequence);
        free(s->revision);free(s->state);s->revision=copy(next);s->state=copy(state);libai_execution_result_set(out,s->revision,"","");
    }else {assert(strcmp(op,"close")==0);s->leased=0;}
}
static libai_agent *bound(struct model_state *m,libai_mcp *c){libai_agent *a=libai_agent_create(model,m,"");assert(a);assert(libai_agent_add_mcp_tool(a,c,"echo","local_echo",policy)==0);return a;}
int main(int argc,char **argv){
    struct transport_state s={0};assert(!libai_mcp_create(NULL,&s,release) && s.released==0);
    libai_mcp *c=connect(&s);char *value=libai_mcp_call(c,"call_tool","{\"name\":\"echo\",\"arguments_json\":\"{}\"}",NULL);assert(value && !strstr(value,"secret"));libai_string_free(value);
    assert(!libai_mcp_call(c,"list_tools","{\"timeout_ms\":0}",NULL));assert(!libai_mcp_call(c,"list_tools","{\"extra\":true}",NULL));
    struct model_state denied={0};libai_agent *a=bound(&denied,c);value=libai_agent_run(a,"echo",NULL,NULL);assert(value && strstr(value,"completed") && s.called==1);libai_string_free(value);assert(libai_agent_destroy(a)==0);
    struct model_state invalid={0};invalid.invalid=1;a=bound(&invalid,c);libai_hooks hooks={0};hooks.authorize=authorize;hooks.user=&invalid;
    value=libai_agent_run(a,"echo",NULL,&hooks);assert(value && invalid.authorized==0 && s.called==1);libai_string_free(value);assert(libai_agent_destroy(a)==0);
    struct model_state success={0};struct store saved={0};a=bound(&success,c);hooks.user=&success;
    assert(libai_agent_set_execution_store(a,"c-mcp",storage,&saved)==0);
    /* Agent owns callback transport after destroying the outer handle. */
    assert(libai_mcp_destroy(c)==0);s.handle=NULL;assert(s.released==0 && s.closed==0);
    value=libai_agent_run(a,"echo",NULL,&hooks);assert(value && strstr(value,"completed") && s.called==2 && strcmp(s.key,"c-mcp.0")==0);libai_string_free(value);
    value=libai_agent_run(a,"echo",NULL,&hooks);assert(value && s.called==2 && success.calls==2);libai_string_free(value);
    assert(libai_agent_destroy(a)==0 && s.released==1 && s.closed==1);free(saved.state);free(saved.revision);
    struct transport_state lost={0};struct store unknown={0};struct model_state attempts={0};c=connect(&lost);lost.lose=1;a=bound(&attempts,c);hooks.user=&attempts;
    assert(libai_agent_set_execution_store(a,"c-unknown",storage,&unknown)==0);lost.handle=NULL;
    value=libai_agent_run(a,"write",NULL,&hooks);assert(value && strstr(value,"recovery_required") && lost.called==1);libai_string_free(value);
    assert(libai_agent_destroy(a)==0 && libai_mcp_destroy(c)==0 && lost.released==1);
    struct transport_state fresh={0};c=connect(&fresh);a=bound(&attempts,c);assert(libai_agent_set_execution_store(a,"c-unknown",storage,&unknown)==0);
    value=libai_agent_run(a,"write",NULL,&hooks);assert(value && strstr(value,"recovery_required") && fresh.called==0 && attempts.calls==1);libai_string_free(value);
    assert(libai_agent_destroy(a)==0 && libai_mcp_destroy(c)==0);free(unknown.state);free(unknown.revision);
    for(int kind=0;kind<3;++kind){struct transport_state failure={0};failure.missing=kind==0;failure.duplicate=kind==1;failure.oversize=kind==2;
        c=libai_mcp_create(transport,&failure,release);assert(c);assert(!libai_mcp_call(c,"initialize","{\"name\":\"test\",\"version\":\"1\"}",NULL));assert(failure.closed==1);assert(libai_mcp_destroy(c)==0 && failure.released==1);}
    struct transport_state cancelled={0};c=connect(&cancelled);cancelled.stop=1;cancelled.token=libai_cancellation_create();
    assert(!libai_mcp_call(c,"call_tool","{\"name\":\"echo\",\"arguments_json\":\"{}\"}",cancelled.token));
    assert(libai_cancellation_cancelled(cancelled.token) && cancelled.cancelled==1 && cancelled.closed==1);
    assert(libai_mcp_destroy(c)==0 && cancelled.released==1);libai_cancellation_destroy(cancelled.token);
    if(argc==3){assert(libai_mcp_stdio_available());char config[8192];snprintf(config,sizeof(config),"{\"executable\":\"%s\",\"arguments\":[\"%s\"]}",argv[1],argv[2]);
        c=libai_mcp_create_stdio(config);assert(c);value=libai_mcp_call(c,"initialize","{\"name\":\"stdio\",\"version\":\"1\"}",NULL);assert(value);libai_string_free(value);
        value=libai_mcp_call(c,"list_tools","{}",NULL);assert(value);libai_string_free(value);struct model_state process={0};a=bound(&process,c);hooks.user=&process;
        value=libai_agent_run(a,"echo",NULL,&hooks);assert(value && strstr(value,"completed"));libai_string_free(value);
        assert(libai_agent_destroy(a)==0 && libai_mcp_close(c)==0 && libai_mcp_close(c)==0 && libai_mcp_destroy(c)==0);
    }
}
