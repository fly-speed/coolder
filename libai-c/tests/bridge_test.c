#include "libai.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Drive a real C++ tool loop through the C ABI without network credentials. */
static void model(void *user, const char *request, libai_response *response,
    libai_invocation *invocation)
{
	int *turn = user;
	assert(strstr(request, "\"messages\"") != NULL);
	if ((*turn)++ == 0)
		libai_response_call(
		    response, "call_1", "status", "{\"limit\":2}");
	else {
		assert(strstr(request, "ready") != NULL);
		libai_response_text(response, "ready", "");
		assert(libai_invocation_emit(invocation, "ready"));
	}
	libai_response_usage(response, 5, 2);
}
/* Foreign tools copy their output into native storage before returning. */
static void tool(void *user, const char *id, const char *name, const char *args,
    libai_tool_result *result, libai_invocation *invocation)
{
	(void)user;
	assert(strcmp(id, "call_1") == 0 && strcmp(name, "status") == 0);
	assert(strstr(args, "limit") != NULL &&
	    !libai_invocation_stopped(invocation));
	libai_tool_result_set(result, "ready", 0);
}
static int stream(void *user, const char *text)
{
	int *count = user;
	++*count;
	assert(strcmp(text, "ready") == 0);
	return 1;
}
int main(void)
{
	int turn = 0, streamed = 0;
	assert(libai_abi_version() == 1);
	assert(libai_agent_create(NULL, NULL, "") == NULL);
	libai_agent *agent = libai_agent_create(model, &turn, "test");
	assert(agent);
	assert(libai_agent_add_tool(
	           agent, "status", "Get status", NULL, 0, tool, NULL) == 0);
	assert(libai_agent_add_tool(
	           agent, "status", "Duplicate", NULL, 0, tool, NULL) == -1);
	libai_options options;
	libai_options_default(&options);
	libai_hooks hooks = { &streamed, stream, NULL, NULL };
	char *result = libai_agent_run(agent, "start", &options, &hooks);
	assert(result && strstr(result, "\"status\":\"completed\"") &&
	    strstr(result, "\"tool_calls\":1"));
	assert(streamed == 1);
	libai_string_free(result);
	assert(libai_agent_reset(agent) == 0);
	turn = 0;
	options.max_tool_calls = 0;
	result = libai_agent_run(agent, "start", &options, NULL);
	assert(result && strstr(result, "budget_exceeded"));
	libai_string_free(result);
	assert(libai_agent_destroy(agent) == 0);
	puts("C ABI tests passed");
}
