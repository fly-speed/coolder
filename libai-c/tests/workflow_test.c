#include <libai.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <pthread.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <time.h>
#endif
struct node_state {libai_workflow *graph;const char *output;int calls,released,mode;};
static void release(void *user){++((struct node_state*)user)->released;}
static void emit(libai_workflow_result *out,const char *json){assert(libai_workflow_result_set(out,json,strlen(json))==0);}
static void callback(void *user,const char *state,const char *context,libai_workflow_result *out,libai_invocation *control){
    struct node_state *s=user;++s->calls;assert(control&&context&&state);
    assert(!*libai_invocation_idempotency_key(control));
    if(s->graph){
        assert(libai_workflow_destroy(s->graph)==-1);
        assert(!libai_workflow_run(s->graph,"{}","{}",NULL,NULL,NULL));
        assert(libai_workflow_add_edge(s->graph,"a","b","")==-1);
    }
    if(s->mode==1)return;
    if(s->mode==2){assert(libai_workflow_result_set(out,"{}",16*1024*1024+1)==-1);return;}
    if(s->mode==3){assert(libai_workflow_result_set(out,"{\"status\":\"bad\"}",16)==-1);return;}
    if(s->mode==4){emit(out,"{}");assert(libai_workflow_result_set(out,"{}",2)==-1);return;}
    if(s->mode==5){assert(libai_workflow_result_set(out,"{\"bad\":1}",9)==-1);return;}
    if(s->mode==6){while(!libai_invocation_stopped(control)){};emit(out,"{}");return;}
    if(s->mode==7){assert(strstr(state,"a")&&strstr(state,"b"));}
    emit(out,s->output?s->output:"{}");
}
static void add(libai_workflow *g,const char *descriptor,struct node_state *s){assert(libai_workflow_add_node(g,descriptor,callback,s,release)==0);}
static char *run(libai_workflow *g,const char *options){char *s=libai_workflow_run(g,"{}",options,NULL,NULL,NULL);assert(s);return s;}
static void basic_and_failures(void){
    libai_workflow *g=libai_workflow_create();assert(g);struct node_state choose={0},yes={0},no={0},join={0};
    choose.output="{\"route\":\"yes\"}";yes.output="{\"patch_json\":\"{\\\"answer\\\":42}\"}";
    choose.graph=g;
    assert(libai_workflow_add_node(g,"{\"name\":\"choose\",\"bad\":1}",callback,&choose,release)==-1&&choose.released==0);
    assert(libai_workflow_add_node(g,"{\"name\":\"choose\"}",NULL,&choose,release)==-1&&choose.released==0);
    add(g,"{\"name\":\"choose\"}",&choose);
    assert(libai_workflow_add_node(g,"{\"name\":\"choose\"}",callback,&choose,release)==-1&&choose.released==0);
    add(g,"{\"name\":\"yes\",\"writes\":[\"answer\"]}",&yes);add(g,"{\"name\":\"no\"}",&no);add(g,"{\"name\":\"join\"}",&join);
    assert(libai_workflow_add_edge(g,"choose","yes","yes")==0);assert(libai_workflow_add_edge(g,"choose","no","no")==0);
    assert(libai_workflow_add_edge(g,"yes","join",NULL)==0);assert(libai_workflow_add_edge(g,"no","join","")==0);
    assert(!libai_workflow_run(g,"{}","{\"max_parallel\":0}",NULL,NULL,NULL)&&choose.calls==0);
    assert(!libai_workflow_run(g,"{}","{\"timeout_ms\":-1}",NULL,NULL,NULL));
    assert(!libai_workflow_run(g,"{}","{\"scope\":\"x\\u0000y\"}",NULL,NULL,NULL));
    char *v=run(g,"{\"scope\":\"job\"}");assert(strstr(v,"\"status\":\"completed\"")&&strstr(v,"\"accepted\":false")&&strstr(v,"\"no\":\"skipped\""));
    assert(choose.calls==1&&yes.calls==1&&no.calls==0&&join.calls==1);libai_string_free(v);
    assert(libai_workflow_destroy(g)==0&&choose.released==1&&yes.released==1&&no.released==1&&join.released==1);
    int mode;for(mode=1;mode<=5;++mode){struct node_state s={0};s.mode=mode;g=libai_workflow_create();add(g,"{\"name\":\"a\"}",&s);
        v=run(g,"{}");assert(strstr(v,"\"status\":\"callback_error\"")&&s.calls==1);libai_string_free(v);assert(libai_workflow_destroy(g)==0&&s.released==1);
    }
    struct node_state wait={0};wait.mode=6;g=libai_workflow_create();add(g,"{\"name\":\"wait\"}",&wait);
    v=run(g,"{\"timeout_ms\":5}");assert(strstr(v,"\"status\":\"deadline_exceeded\""));libai_string_free(v);
    libai_cancellation *token=libai_cancellation_create();libai_cancellation_cancel(token);
    v=libai_workflow_run(g,"{}","{}",NULL,NULL,token);assert(v&&strstr(v,"\"status\":\"cancelled\"")&&wait.calls==1);libai_string_free(v);
    libai_cancellation_destroy(token);assert(libai_workflow_destroy(g)==0);
    struct node_state conflict_a={0},conflict_b={0};g=libai_workflow_create();
    add(g,"{\"name\":\"a\",\"writes\":[\"x\"],\"parallel_safe\":true}",&conflict_a);
    add(g,"{\"name\":\"b\",\"writes\":[\"x\"],\"parallel_safe\":true}",&conflict_b);
    v=run(g,"{\"max_parallel\":2}");assert(strstr(v,"\"status\":\"protocol_error\"")&&conflict_a.calls==0&&conflict_b.calls==0);libai_string_free(v);assert(libai_workflow_destroy(g)==0);
}
struct store {char *revision,*state;int leased,sequence;};
static char *copy(const char *s){char *r=malloc(strlen(s)+1);assert(r);strcpy(r,s);return r;}
static void storage(void *user,const char *op,const char *scope,const char *revision,const char *state,libai_execution_result *out){
    struct store *s=user;assert(strcmp(scope,"wfg3_job")==0);
    if(strcmp(op,"open")==0){assert(!s->leased);s->leased=1;libai_execution_result_set(out,s->revision?s->revision:"",s->state?s->state:"","");}
    else if(strcmp(op,"commit")==0){char next[32];assert(s->leased&&strcmp(revision,s->revision?s->revision:"")==0);snprintf(next,sizeof(next),"%d",++s->sequence);free(s->revision);free(s->state);s->revision=copy(next);s->state=copy(state);libai_execution_result_set(out,s->revision,"","");}
    else{assert(strcmp(op,"close")==0);s->leased=0;}
}
static void durable(void){
    struct store s={0};struct node_state a={0},b={0};a.output="{\"patch_json\":\"{\\\"a\\\":1}\"}";b.output="{\"error\":\"lost acknowledgement\"}";
    libai_workflow *g=libai_workflow_create();add(g,"{\"name\":\"a\",\"writes\":[\"a\"]}",&a);add(g,"{\"name\":\"b\",\"writes\":[\"b\"]}",&b);assert(libai_workflow_add_edge(g,"a","b","")==0);
    char *v=libai_workflow_run(g,"{}","{\"scope\":\"job\"}",storage,&s,NULL);assert(v&&strstr(v,"\"status\":\"recovery_required\"")&&a.calls==1&&b.calls==1&&!s.leased);libai_string_free(v);
    v=libai_workflow_run(g,"{}","{\"scope\":\"job\"}",storage,&s,NULL);assert(v&&a.calls==1&&b.calls==1);libai_string_free(v);
    assert(libai_workflow_confirm_node(g,storage,&s,"job","{}","stale","b","applied","{}","receipt/1")==-1);
    assert(libai_workflow_confirm_node(g,storage,&s,"job","{}",s.revision,"b","applied","{\"patch_json\":\"{\\\"outside\\\":2}\"}","receipt/1")==-1);
    assert(libai_workflow_confirm_node(g,storage,&s,"job","{}",s.revision,"b","applied","{\"patch_json\":\"{\\\"b\\\":2}\"}","receipt/1")==0);
    v=libai_workflow_run(g,"{}","{\"scope\":\"job\"}",storage,&s,NULL);assert(v&&strstr(v,"\"status\":\"completed\"")&&a.calls==1&&b.calls==1);libai_string_free(v);
    v=libai_workflow_run(g,"{}","{\"scope\":\"job\"}",storage,&s,NULL);assert(v&&a.calls==1&&b.calls==1);libai_string_free(v);
    v=libai_workflow_run(g,"{\"changed\":1}","{\"scope\":\"job\"}",storage,&s,NULL);assert(v&&strstr(v,"\"progress_available\":false")&&strstr(v,"recovery_required"));libai_string_free(v);
    assert(libai_workflow_destroy(g)==0&&!s.leased);free(s.revision);free(s.state);
}

struct child_state {struct store store;int models,effects;};
static void child_model(void *user,const char *request,libai_response *out,libai_invocation *control){
    struct child_state *s=user;(void)request;assert(control&&!libai_invocation_stopped(control));
    if(++s->models==1)libai_response_call(out,"call1","effect","{}");
    else libai_response_text(out,"done","");
}
static void child_tool(void *user,const char *id,const char *name,const char *args,libai_tool_result *out,libai_invocation *control){
    struct child_state *s=user;(void)id;(void)name;(void)args;++s->effects;
    assert(strcmp(libai_invocation_idempotency_key(control),"wf3_job_agent.0")==0);
    libai_tool_result_set(out,"3",0);
}
static void child_storage(void *user,const char *op,const char *scope,const char *revision,const char *state,libai_execution_result *out){
    assert(strcmp(scope,"wf3_job_agent")==0);storage(user,op,"wfg3_job",revision,state,out);
}
static void child_node(void *user,const char *state,const char *context,libai_workflow_result *out,libai_invocation *control){
    struct child_state *s=user;assert(strstr(context,"wf3_job_agent"));
    libai_cancellation *token=libai_invocation_retain_cancellation(control);assert(token&&!libai_cancellation_cancelled(token));
    libai_options options;libai_options_default(&options);int64_t remaining=libai_invocation_remaining_ms(control);assert(remaining>0);
    options.timeout_ms=remaining;
    libai_agent *agent=libai_agent_create(child_model,s,"");assert(agent);
    assert(libai_agent_add_tool(agent,"effect","","{\"type\":\"object\"}",0,child_tool,s)==0);
    assert(libai_agent_set_execution_store(agent,"wf3_job_agent",child_storage,&s->store)==0);
    char *result=libai_agent_run_with_cancellation(agent,state,&options,NULL,token);assert(result);
    if(strstr(result,"\"status\":\"completed\""))emit(out,"{\"patch_json\":\"{\\\"answer\\\":3}\"}");
    else{assert(strstr(result,"recovery_required"));emit(out,"{\"status\":\"recovery_required\"}");}
    libai_string_free(result);assert(libai_agent_destroy(agent)==0);libai_cancellation_destroy(token);
}
static void agent_composition(void){
    struct child_state s={0};libai_workflow *g=libai_workflow_create();
    assert(libai_workflow_add_node(g,"{\"name\":\"agent\",\"writes\":[\"answer\"]}",child_node,&s,NULL)==0);
    libai_cancellation *token=libai_cancellation_create();assert(token);
    char *v=libai_workflow_run(g,"{}","{\"scope\":\"job\"}",NULL,NULL,token);assert(v&&strstr(v,"\"status\":\"completed\""));libai_string_free(v);
    v=libai_workflow_run(g,"{}","{\"scope\":\"job\"}",NULL,NULL,token);assert(v&&strstr(v,"\"status\":\"completed\"")&&s.models==2&&s.effects==1);libai_string_free(v);
    v=libai_workflow_run(g,"{\"changed\":true}","{\"scope\":\"job\"}",NULL,NULL,token);assert(v&&strstr(v,"\"status\":\"recovery_required\"")&&s.effects==1);libai_string_free(v);
    assert(libai_workflow_destroy(g)==0);libai_cancellation_destroy(token);free(s.store.revision);free(s.store.state);
}

#if defined(__unix__) || defined(__APPLE__)
struct parallel {pthread_mutex_t lock;pthread_cond_t changed;int started;};
struct branch {struct parallel *p;const char *output;};
static void parallel_callback(void *user,const char *state,const char *context,libai_workflow_result *out,libai_invocation *control){
    struct branch *b=user;(void)context;assert(control&&strcmp(state,"{}")==0);
    struct timespec deadline;clock_gettime(CLOCK_REALTIME,&deadline);deadline.tv_sec+=3;
    assert(pthread_mutex_lock(&b->p->lock)==0);++b->p->started;assert(pthread_cond_broadcast(&b->p->changed)==0);
    while(b->p->started<2)assert(pthread_cond_timedwait(&b->p->changed,&b->p->lock,&deadline)==0);
    assert(pthread_mutex_unlock(&b->p->lock)==0);emit(out,b->output);
}
static void parallel(void){
    struct parallel p={PTHREAD_MUTEX_INITIALIZER,PTHREAD_COND_INITIALIZER,0};
    struct branch a={&p,"{\"patch_json\":\"{\\\"a\\\":1}\"}"},b={&p,"{\"patch_json\":\"{\\\"b\\\":2}\"}"};struct node_state joined={0};joined.mode=7;
    libai_workflow *g=libai_workflow_create();assert(libai_workflow_add_node(g,"{\"name\":\"a\",\"writes\":[\"a\"],\"parallel_safe\":true}",parallel_callback,&a,NULL)==0);
    assert(libai_workflow_add_node(g,"{\"name\":\"b\",\"writes\":[\"b\"],\"parallel_safe\":true}",parallel_callback,&b,NULL)==0);add(g,"{\"name\":\"join\"}",&joined);
    assert(libai_workflow_add_edge(g,"a","join","")==0&&libai_workflow_add_edge(g,"b","join","")==0);
    char *v=run(g,"{\"max_parallel\":2}");assert(strstr(v,"\"status\":\"completed\"")&&p.started==2&&joined.calls==1);libai_string_free(v);
    assert(libai_workflow_destroy(g)==0&&pthread_cond_destroy(&p.changed)==0&&pthread_mutex_destroy(&p.lock)==0);
}
struct file_store {libai_file_execution_store *store;char *last_revision;};
static void file_storage(void *user,const char *op,const char *scope,const char *revision,const char *state,libai_execution_result *out){
    struct file_store *s=user;
    if(strcmp(op,"open")==0){char *r=NULL,*v=NULL;int rc=libai_file_execution_store_open_values(s->store,scope,&r,&v);libai_execution_result_set(out,r?r:"",v?v:"",rc?libai_last_error():"");if(!rc){free(s->last_revision);s->last_revision=copy(r);}libai_string_free(r);libai_string_free(v);}
    else if(strcmp(op,"commit")==0){char *r=libai_file_execution_store_commit(s->store,scope,revision,state);libai_execution_result_set(out,r?r:"","",r?"":libai_last_error());if(r){free(s->last_revision);s->last_revision=copy(r);}libai_string_free(r);}
    else{assert(strcmp(op,"close")==0);assert(libai_file_execution_store_close(s->store,scope)==0);}
}
struct effect {char path[256];int die,calls;};
static void effect_callback(void *user,const char *state,const char *context,libai_workflow_result *out,libai_invocation *control){
    struct effect *s=user;(void)state;assert(control&&strstr(context,"wf3_job_write"));++s->calls;
    int fd=open(s->path,O_WRONLY|O_CREAT|O_APPEND,0600);assert(fd>=0&&write(fd,"once",4)==4&&fsync(fd)==0&&close(fd)==0);
    if(s->die)_exit(71);emit(out,"{}");
}
static void process_exit(void){
    char root[]="/tmp/libai-c-workflow-XXXXXX";assert(mkdtemp(root));struct effect e={0};snprintf(e.path,sizeof(e.path),"%s/effect",root);
    pid_t child=fork();assert(child>=0);
    if(!child){e.die=1;struct file_store s={libai_file_execution_store_create_plaintext(root),NULL};assert(s.store);libai_workflow *g=libai_workflow_create();assert(libai_workflow_add_node(g,"{\"name\":\"write\"}",effect_callback,&e,NULL)==0);libai_workflow_run(g,"{}","{\"scope\":\"job\"}",file_storage,&s,NULL);_exit(72);}
    int status=0;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==71);
    struct file_store s={libai_file_execution_store_create_plaintext(root),NULL};assert(s.store);libai_workflow *g=libai_workflow_create();assert(libai_workflow_add_node(g,"{\"name\":\"write\"}",effect_callback,&e,NULL)==0);
    char *v=libai_workflow_run(g,"{}","{\"scope\":\"job\"}",file_storage,&s,NULL);assert(v&&strstr(v,"recovery_required")&&e.calls==0);libai_string_free(v);
    assert(libai_workflow_confirm_node(g,file_storage,&s,"job","{}",s.last_revision,"write","applied","{}","verified/effect")==0);
    v=libai_workflow_run(g,"{}","{\"scope\":\"job\"}",file_storage,&s,NULL);assert(v&&strstr(v,"\"status\":\"completed\"")&&e.calls==0);libai_string_free(v);
    char effect[8]={0};int fd=open(e.path,O_RDONLY);assert(fd>=0&&read(fd,effect,sizeof(effect))==4&&strcmp(effect,"once")==0&&close(fd)==0);
    assert(libai_workflow_destroy(g)==0&&libai_file_execution_store_destroy(s.store)==0);free(s.last_revision);assert(unlink(e.path)==0);
    char path[256];snprintf(path,sizeof(path),"%s/wfg3_job.state",root);assert(unlink(path)==0);snprintf(path,sizeof(path),"%s/wfg3_job.lock",root);assert(unlink(path)==0);assert(rmdir(root)==0);
}
#endif
int main(void){basic_and_failures();durable();agent_composition();
#if defined(__unix__) || defined(__APPLE__)
parallel();process_exit();
#endif
return 0;}
