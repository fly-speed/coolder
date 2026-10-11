#import "LAIInternal.h"

@implementation LAIAgent
- (instancetype)initWithModel:(LAIModel)model
                 instructions:(NSString *)instructions
                        error:(NSError **)error
{
	if (!(self = [super init]))
		return nil;
	if (!model) {
		lai_error(error, @"Model is required");
		return nil;
	}
	_model = [model copy];
	_tools = [NSMutableDictionary new];
	@try {
		_native = libai_agent_create(
		    lai_model, (__bridge void *)self, lai_utf8(instructions));
	} @catch (NSException *exception) {
		lai_error(error, exception.description);
		return nil;
	}
	if (!_native) {
		lai_error(
		    error, [NSString stringWithUTF8String:libai_last_error()]);
		return nil;
	}
	return self;
}
- (instancetype)initWithProvider:(NSDictionary<NSString *, id> *)config
                          apiKey:(NSString *)apiKey
                    instructions:(NSString *)instructions
                           error:(NSError **)error
{
	if (!(self = [super init]))
		return nil;
	_tools = [NSMutableDictionary new];
	NSData *data = [NSJSONSerialization dataWithJSONObject:config
	                                               options:0
	                                                 error:error];
	if (!data)
		return nil;
	NSString *json = [[NSString alloc] initWithData:data
	                                       encoding:NSUTF8StringEncoding];
	@try {
		_native = libai_agent_create_provider(
		    lai_utf8(json), lai_utf8(apiKey), lai_utf8(instructions));
	} @catch (NSException *exception) {
		lai_error(error, exception.description);
		return nil;
	}
	if (!_native) {
		lai_error(
		    error, [NSString stringWithUTF8String:libai_last_error()]);
		return nil;
	}
	return self;
}
- (NSArray<NSDictionary<NSString *, id> *> *)messages:(NSError **)error
{
	@synchronized(self) {
		if (![self idle:error])
			return nil;
		char *raw = libai_agent_messages(_native);
		if (!raw) {
			lai_error(error,
			    [NSString stringWithUTF8String:libai_last_error()]);
			return nil;
		}
		NSData *data = [[NSString stringWithUTF8String:raw]
		    dataUsingEncoding:NSUTF8StringEncoding];
		libai_string_free(raw);
		return [NSJSONSerialization JSONObjectWithData:data
		                                       options:0
		                                         error:error];
	}
}
- (BOOL)idle:(NSError **)error
{
	if (!_native)
		return lai_error(error, @"Agent is closed");
	if (_busy)
		return lai_error(error, @"Agent is running");
	return YES;
}
- (BOOL)addTool:(NSString *)name
         description:(NSString *)description
              schema:(NSString *)schema
    requiresApproval:(BOOL)approval
             handler:(LAITool)handler
               error:(NSError **)error
{
	@synchronized(self) {
		if (![self idle:error])
			return NO;
		if (!handler)
			return lai_error(error, @"Handler is required");
		@try {
			if (libai_agent_add_tool(_native, lai_utf8(name),
			        lai_utf8(description), lai_utf8(schema),
			        approval, lai_tool, (__bridge void *)self))
				return lai_error(error,
				    [NSString stringWithUTF8String:
				                  libai_last_error()]);
			_tools[name] = [handler copy];
			return YES;
		} @catch (NSException *exception) {
			return lai_error(error, exception.description);
		}
	}
}
- (NSDictionary<NSString *, id> *)run:(NSString *)input
                              options:(LAIRunOptions *)options
                                hooks:(LAIRunHooks *)hooks
                                error:(NSError **)error
{
	LAIAgent *keepAlive __attribute__((objc_precise_lifetime)) = self;
	(void)keepAlive;
	@synchronized(self) {
		if (![self idle:error])
			return nil;
		_hooks = [LAIRunHooks new];
		_hooks.onText = hooks.onText;
		_hooks.authorize = hooks.authorize;
		_hooks.onEvent = hooks.onEvent;
		_busy = YES;
	}
	libai_cancellation *lease = NULL;
	@try {
		options = options ?: [LAIRunOptions new];
		libai_options limits = { options.maxModelCalls,
			options.maxToolCalls, options.maxOutputTokens,
			options.maxTokensPerCall, options.maxContextBytes,
			options.maxToolResultBytes, options.maxModelRetries,
			options.timeoutMs };
		libai_hooks observers = { (__bridge void *)self,
			_hooks.onText ? lai_text : NULL,
			_hooks.authorize ? lai_authorize : NULL,
			_hooks.onEvent ? lai_event : NULL };
		if (options.cancellation) {
			lease = [options.cancellation retainNative];
			if (!lease) {
				lai_error(error,
				    [NSString stringWithUTF8String:
				                  libai_last_error()]);
				return nil;
			}
		}
		char *raw = libai_agent_run_with_cancellation(
		    _native, lai_utf8(input), &limits, &observers, lease);
		if (!raw) {
			lai_error(error,
			    [NSString stringWithUTF8String:libai_last_error()]);
			return nil;
		}
		NSData *json = [[NSString stringWithUTF8String:raw]
		    dataUsingEncoding:NSUTF8StringEncoding];
		libai_string_free(raw);
		return [NSJSONSerialization JSONObjectWithData:json
		                                       options:0
		                                         error:error];
	} @catch (NSException *exception) {
		lai_error(error, exception.description);
		return nil;
	} @finally {
		libai_cancellation_destroy(lease);
		@synchronized(self) {
			_busy = NO;
			_hooks = nil;
		}
	}
}
- (void)cancel
{
	@synchronized(self) {
		if (_native)
			libai_agent_cancel(_native);
	}
}
- (BOOL)reset:(NSError **)error
{
	@synchronized(self) {
		if (![self idle:error])
			return NO;
		if (libai_agent_reset(_native))
			return lai_error(error,
			    [NSString stringWithUTF8String:libai_last_error()]);
		return YES;
	}
}
- (BOOL)close:(NSError **)error
{
	@synchronized(self) {
		if (!_native)
			return YES;
		if (![self idle:error])
			return NO;
		if (libai_agent_destroy(_native))
			return lai_error(error,
			    [NSString stringWithUTF8String:libai_last_error()]);
		_native = NULL;
		[_tools removeAllObjects];
		return YES;
	}
}
- (void)dealloc
{
	if (_native)
		libai_agent_destroy(_native);
}
@end
