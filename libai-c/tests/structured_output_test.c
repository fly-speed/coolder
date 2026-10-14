#include "libai.h"
#include <assert.h>
#include <string.h>
static const char *schema="{\"type\":\"object\",\"required\":[\"n\"],\"properties\":{\"n\":{\"type\":\"integer\"}}}";
struct state {int turn,effects,visible,plain;libai_agent *agent;};
static void model(void *user,const char *request,libai_response *out,libai_invocation *inv) {
 struct state *s=user;(void)inv;
 if(s->plain){assert(!strstr(request,"\"output_schema\""));libai_response_text(out,"plain","");return;}
 assert(strstr(request,"\"output_schema\""));
 assert(libai_agent_configure_output(s->agent,"",0)==-1);
 if(++s->turn==1)libai_response_call(out,"call","write","{}");
 else if(s->turn==2)libai_response_text(out,"{}","");
 else {assert(strstr(request,"\"output_repair\":true") && strstr(request,"\"tools\":[]"));libai_response_text(out,"{\"n\":2}","");}
}
static void tool(void *user,const char *id,const char *name,const char *args,libai_tool_result *out,libai_invocation *inv) {
 (void)id;(void)name;(void)args;(void)inv;++((struct state*)user)->effects;libai_tool_result_set(out,"ok",0);
}
static int stream(void *user,const char *text){++((struct state*)user)->visible;assert(!strcmp(text,"{\"n\":2}"));return 1;}
int main(void){
 struct state s={0};s.agent=libai_agent_create(model,&s,"");assert(s.agent);
 assert(!libai_agent_configure_output(s.agent,schema,1));
 assert(libai_agent_configure_output(s.agent,"{\"const\":1}",1)==-1);
 assert(libai_agent_configure_output(s.agent,"",9)==-1);
 assert(!libai_agent_add_tool(s.agent,"write","write","{}",0,tool,&s));
 libai_options options;libai_options_default(&options);libai_hooks hooks={&s,stream,NULL,NULL};
 char *result=libai_agent_run(s.agent,"work",&options,&hooks);
 assert(result && strstr(result,"\"output_validated\":true") && strstr(result,"\"output_repairs\":1"));
 assert(strstr(result,"\"accepted\":false") && s.turn==3 && s.effects==1 && s.visible==1);libai_string_free(result);
 s.plain=1;assert(!libai_agent_configure_output(s.agent,"",0));result=libai_agent_run(s.agent,"plain",&options,NULL);
 assert(result && strstr(result,"\"output_validated\":false") && strstr(result,"\"output_repairs\":0"));libai_string_free(result);
 assert(!libai_agent_destroy(s.agent));
}
