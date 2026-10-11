#import "LAIInternal.h"

void lai_model(void *user, const char *request, libai_response *output,
    libai_invocation *boundary)
{
	@autoreleasepool {
		LAIAgent *agent = (__bridge LAIAgent *)user;
		LAIInvocation *invocation = [LAIInvocation new];
		invocation.pointer = boundary;
		invocation.thread = [NSThread currentThread];
		@try {
			LAIModelResponse *response =
			    agent.model([NSString stringWithUTF8String:request],
			        invocation);
			if (!response)
				[NSException raise:NSInvalidArgumentException
				            format:@"Model returned nil"];
			libai_response_text(output, lai_utf8(response.text),
			    lai_utf8(response.reasoning));
			libai_response_usage(output, response.inputTokens,
			    response.outputTokens);
			libai_response_error(output, lai_utf8(response.error),
			    response.retryable);
			for (LAIToolCall *call in response.calls)
				libai_response_call(output,
				    lai_utf8(call.identifier),
				    lai_utf8(call.name),
				    lai_utf8(call.arguments));
		} @catch (NSException *error) {
			libai_response_error(
			    output, error.description.UTF8String, 0);
		} @finally {
			invocation.pointer = NULL;
		}
	}
}
void lai_tool(void *user, const char *id, const char *name,
    const char *arguments, libai_tool_result *output,
    libai_invocation *boundary)
{
	@autoreleasepool {
		LAIAgent *agent = (__bridge LAIAgent *)user;
		LAIInvocation *invocation = [LAIInvocation new];
		invocation.pointer = boundary;
		invocation.thread = [NSThread currentThread];
		@try {
			LAIToolCall *call = [LAIToolCall new];
			call.identifier = [NSString stringWithUTF8String:id];
			call.name = [NSString stringWithUTF8String:name];
			call.arguments =
			    [NSString stringWithUTF8String:arguments];
			LAIToolResult *result =
			    agent.tools[call.name](call, invocation);
			if (!result)
				[NSException raise:NSInvalidArgumentException
				            format:@"Tool returned nil"];
			libai_tool_result_set(
			    output, lai_utf8(result.text), result.error);
		} @catch (NSException *error) {
			libai_tool_result_set(
			    output, error.description.UTF8String, 1);
		} @finally {
			invocation.pointer = NULL;
		}
	}
}
int lai_text(void *user, const char *text)
{
	LAIAgent *agent = (__bridge LAIAgent *)user;
	@try {
		return agent.hooks.onText([NSString stringWithUTF8String:text]);
	} @catch (NSException *) {
		[agent cancel];
		return 0;
	}
}
int lai_authorize(
    void *user, const char *name, const char *arguments, int required)
{
	LAIAgent *agent = (__bridge LAIAgent *)user;
	@try {
		return agent.hooks.authorize(
		    [NSString stringWithUTF8String:name],
		    [NSString stringWithUTF8String:arguments], required);
	} @catch (NSException *) {
		[agent cancel];
		return 0;
	}
}
void lai_event(void *user, const char *text)
{
	LAIAgent *agent = (__bridge LAIAgent *)user;
	@try {
		agent.hooks.onEvent([NSString stringWithUTF8String:text]);
	} @catch (NSException *) {
		[agent cancel];
	}
}
