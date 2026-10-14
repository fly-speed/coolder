#import "LAIInternal.h"
#import "LAIMcpInternal.h"

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
- (BOOL)configureOutput:(NSDictionary *)schema repairs:(NSUInteger)repairs error:(NSError **)error
{
    @synchronized(self) {
        if (![self idle:error]) return NO;
        NSString *wire=@"";
        if (schema) {
            NSJSONWritingOptions options=0;
            if (@available(macOS 10.13, iOS 11.0, *)) options=NSJSONWritingSortedKeys;
            else return lai_error(error,@"structured output requires deterministic Foundation JSON serialization");
            NSData *data=[NSJSONSerialization dataWithJSONObject:schema options:options error:error];
            if (!data) return NO;
            wire=[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
        }
        return [self configureOutputJSON:wire repairs:repairs error:error];
    }
}
- (BOOL)configureOutputJSON:(NSString *)schemaJSON repairs:(NSUInteger)repairs error:(NSError **)error
{
    @synchronized(self) {
        if (![self idle:error]) return NO;
        @try {
            if (libai_agent_configure_output(_native,lai_utf8(schemaJSON ?: @""),repairs))
                return lai_error(error,[NSString stringWithUTF8String:libai_last_error()]);
        } @catch(NSException *exception) {return lai_error(error,exception.description);}
        return YES;
    }
}
- (BOOL)configureModel:(NSDictionary<NSString *, id> *)profile error:(NSError **)error
{
	@synchronized(self) {
		if (![self idle:error]) return NO;
		NSData *data = [NSJSONSerialization dataWithJSONObject:profile options:0 error:error];
		if (!data) return NO;
		NSString *wire = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
		@try {
			if (libai_agent_configure_model(_native, lai_utf8(wire)))
				return lai_error(error, [NSString stringWithUTF8String:libai_last_error()]);
		} @catch (NSException *exception) { return lai_error(error, exception.description); }
		return YES;
	}
}
- (BOOL)addTool:(NSString *)name
         description:(NSString *)description
              schema:(NSString *)schema
    requiresApproval:(BOOL)approval
             handler:(LAITool)handler
               error:(NSError **)error
{
    return [self addTool:name description:description schema:schema requiresApproval:approval
        executionRevision:@"1" handler:handler error:error];
}
- (BOOL)addMcpTool:(LAIMcpClient *)client remoteName:(NSString *)remote localName:(NSString *)local policy:(NSDictionary *)policy error:(NSError **)error
{
    @synchronized(self) {
        if (![self idle:error]) return NO;
        if (!client) return lai_error(error,@"MCP client required");
        libai_mcp *native=[client begin:error];if(!native)return NO;
        @try {
            NSMutableDictionary *converted=[policy mutableCopy];
            for(NSString *key in @[@"parameters_schema",@"output_schema"]){
                if([converted[key] isKindOfClass:[NSDictionary class]]){
                    NSJSONWritingOptions options=0;
                    if(@available(macOS 10.13,iOS 11.0,*))options=NSJSONWritingSortedKeys;
                    else return lai_error(error,@"Dictionary MCP Schema requires deterministic JSON encoding");
                    NSData *data=[NSJSONSerialization dataWithJSONObject:converted[key] options:options error:error];if(!data)return NO;
                    converted[key]=[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
                }
            }
            NSData *data=[NSJSONSerialization dataWithJSONObject:converted options:0 error:error];if(!data)return NO;
            NSString *wire=[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
            if(libai_agent_add_mcp_tool(_native,native,lai_utf8(remote),lai_utf8(local),lai_utf8(wire)))
                return lai_error(error,[NSString stringWithUTF8String:libai_last_error()]);
            return YES;
        }@catch(NSException *e){return lai_error(error,e.description);}
        @finally {[client end];}
    }
}
- (BOOL)setExecutionStore:(id<LAIExecutionStore>)store scope:(NSString *)scope error:(NSError **)error
{
    @synchronized(self) {
        if (![self idle:error]) return NO;
        @try {
            if (libai_agent_set_execution_store(_native, lai_utf8(scope), store ? lai_execution : nullptr,
                (__bridge void *)store)) return lai_error(error, [NSString stringWithUTF8String:libai_last_error()]);
            _executionStore = store;
            return YES;
        } @catch (NSException *failure) { return lai_error(error, failure.description); }
    }
}
- (BOOL)addTool:(NSString *)name description:(NSString *)description schema:(NSString *)schema
    requiresApproval:(BOOL)approval executionRevision:(NSString *)revision handler:(LAITool)handler
               error:(NSError **)error
{
    return [self addTool:name description:description schema:schema requiresApproval:approval
        executionRevision:revision sideEffectFree:NO handler:handler error:error];
}
- (BOOL)addTool:(NSString *)name description:(NSString *)description schema:(NSString *)schema
    requiresApproval:(BOOL)approval executionRevision:(NSString *)revision sideEffectFree:(BOOL)sideEffectFree
             handler:(LAITool)handler error:(NSError **)error
{
	@synchronized(self) {
		if (![self idle:error])
			return NO;
		if (!handler)
			return lai_error(error, @"Handler is required");
		@try {
			if (libai_agent_add_tool_governed(_native, lai_utf8(name),
			        lai_utf8(description), lai_utf8(schema),
			        approval, lai_utf8(revision), sideEffectFree, lai_tool, (__bridge void *)self))
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
		_executionStore = nil;
		return YES;
	}
}
- (void)dealloc
{
	if (_native)
		libai_agent_destroy(_native);
}
@end
