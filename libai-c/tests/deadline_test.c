#include "libai.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static char *state;static unsigned revision;static int leased,effects,models;
static char *copy(const char *text){size_t n=strlen(text)+1;char *p=malloc(n);assert(p);memcpy(p,text,n);return p;}
static void storage(void *user,const char *op,const char *scope,const char *expected,const char *wire,libai_execution_result *result){
 (void)user;assert(strcmp(scope,"task")==0);char rev[32];sprintf(rev,"%u",revision);
 if(!strcmp(op,"open")){assert(!leased);leased=1;libai_execution_result_set(result,revision?rev:"",state?state:"","");}
 else if(!strcmp(op,"close")){assert(leased);leased=0;}
 else {assert(leased&&(!revision?!*expected:!strcmp(expected,rev)));free(state);state=copy(wire);sprintf(rev,"%u",++revision);libai_execution_result_set(result,rev,"","");}
}
static void model(void *user,const char *request,libai_response *result,libai_invocation *inv){
 (void)user;(void)inv;++models;if(strstr(request,"\"role\":\"tool\""))libai_response_text(result,"done","");else libai_response_call(result,"write","write","{}");
}
static void tool(void *user,const char *id,const char *name,const char *args,libai_tool_result *result,libai_invocation *inv){
 (void)user;(void)id;(void)name;(void)args;assert(!strcmp(libai_invocation_idempotency_key(inv),"task.0"));++effects;libai_tool_result_set(result,"unknown",1);
}
int main(void){
 libai_agent *agent=libai_agent_create(model,NULL,"");assert(agent);
 assert(!libai_agent_add_tool(agent,"write","","{}",0,tool,NULL));assert(!libai_agent_set_execution_store(agent,"task",storage,NULL));
 char *out=libai_agent_run(agent,"input",NULL,NULL);assert(out&&strstr(out,"recovery_required"));libai_string_free(out);assert(effects==1&&models==1&&!leased);
 assert(!strncmp(state,"{\"time_budget\":{",16));char *end=strstr(state,"},");assert(end);memmove(state+1,end+2,strlen(end+2)+1);
 char *legacy=copy(state);char rev[32];sprintf(rev,"%u",revision);
 assert(libai_execution_adopt_deadline(storage,NULL,"task","input","","stale",1000,"audit")==-1);
 assert(libai_execution_adopt_deadline(storage,NULL,"task","input","",rev,-1,"audit")==-1);
 assert(libai_execution_adopt_deadline(storage,NULL,"task","input","",rev,INT64_MAX,"audit")==-1);
 assert(libai_execution_adopt_deadline(storage,NULL,"task","input","",rev,1000,"")==-1);assert(!strcmp(state,legacy)&&!leased);free(legacy);
 assert(!libai_execution_adopt_deadline(storage,NULL,"task","input","",rev,300000,"host-budget-audit"));assert(strstr(state,"host_adoption"));sprintf(rev,"%u",revision);
 assert(libai_execution_adopt_deadline(storage,NULL,"task","input","",rev,300000,"audit")==-1);
 out=libai_agent_run(agent,"input",NULL,NULL);assert(out&&strstr(out,"recovery_required"));libai_string_free(out);assert(effects==1&&models==1&&!leased);
 assert(!libai_execution_confirm(storage,NULL,"task","input","",rev,"task.0","verified",0,"applied","host-receipt"));
 out=libai_agent_run(agent,"input",NULL,NULL);assert(out&&strstr(out,"completed"));libai_string_free(out);assert(effects==1&&models==2&&!leased);
 libai_agent_destroy(agent);free(state);return 0;
}
