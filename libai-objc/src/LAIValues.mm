#include <cstring>
#import "LAIAgent.h"
#import "LAIInternal.h"

@implementation LAIToolCall
- (instancetype)init
{
	if ((self = [super init])) {
		_identifier = @"";
		_name = @"";
		_arguments = @"{}";
	}
	return self;
}
@end
@implementation LAIModelResponse
- (instancetype)init
{
	if ((self = [super init])) {
		_text = @"";
		_reasoning = @"";
		_error = @"";
		_calls = @[];
	}
	return self;
}
@end
@implementation LAIToolResult
- (instancetype)init
{
	if ((self = [super init]))
		_text = @"";
	return self;
}
@end
@implementation LAIRunHooks
@end
@implementation LAIRunOptions
- (instancetype)init
{
	if ((self = [super init])) {
		libai_options options;
		libai_options_default(&options);
		_maxModelCalls = options.max_model_calls;
		_maxToolCalls = options.max_tool_calls;
		_maxOutputTokens = options.max_output_tokens;
		_maxTokensPerCall = options.max_tokens_per_call;
		_maxContextBytes = options.max_context_bytes;
		_maxToolResultBytes = options.max_tool_result_bytes;
		_maxModelRetries = options.max_model_retries;
		_timeoutMs = options.timeout_ms;
	}
	return self;
}
@end
@implementation LAIInvocation
- (BOOL)stopped
{
	if (!_pointer || _thread != [NSThread currentThread])
		[NSException raise:NSInternalInconsistencyException
		            format:@"Invocation expired"];
	return libai_invocation_stopped(_pointer);
}
- (BOOL)emit:(NSString *)text
{
	if (!_pointer || _thread != [NSThread currentThread])
		[NSException raise:NSInternalInconsistencyException
		            format:@"Invocation expired"];
	return libai_invocation_emit(_pointer, lai_utf8(text));
}
@end

const char *lai_utf8(NSString *text)
{
	const char *bytes = text.UTF8String;
	if (!text || !bytes ||
	    strlen(bytes) !=
	        [text lengthOfBytesUsingEncoding:NSUTF8StringEncoding])
		[NSException raise:NSInvalidArgumentException
		            format:@"Expected UTF-8 text without NUL"];
	return bytes;
}
BOOL lai_error(NSError **error, NSString *message)
{
	if (error)
		*error = [NSError
		    errorWithDomain:@"io.coolder.libai"
		               code:1
		           userInfo:@{
			           NSLocalizedDescriptionKey : message ?:
			               @"Native operation failed"
		           }];
	return NO;
}

@implementation LAICancellation
- (instancetype)init
{
	if ((self = [super init])) {
		_native = libai_cancellation_create();
		if (!_native)
			return nil;
	}
	return self;
}
- (void)cancel
{
	@synchronized(self) {
		libai_cancellation_cancel(_native);
	}
}
- (BOOL)cancelled
{
	@synchronized(self) {
		return libai_cancellation_cancelled(_native);
	}
}
- (libai_cancellation *)retainNative
{
	@synchronized(self) {
		return libai_cancellation_retain(_native);
	}
}
- (void)dealloc
{
	libai_cancellation_destroy(_native);
}
@end
