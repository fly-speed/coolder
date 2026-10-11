#import "LAIAgent.h"
#include <assert.h>
int main(void)
{
	@autoreleasepool {
		__block int turns = 0, tools = 0;
		__block LAIInvocation *saved;
		NSError *error = nil;
		LAIAgent *agent = [[LAIAgent alloc]
		    initWithModel:^LAIModelResponse *(
		        NSString *request, LAIInvocation *invocation) {
		      saved = invocation;
		      LAIModelResponse *response = [LAIModelResponse new];
		      if (turns++ == 0) {
			      LAIToolCall *call = [LAIToolCall new];
			      call.identifier = @"编号";
			      call.name = @"echo";
			      call.arguments = @"{}";
			      response.calls = @[ call ];
		      } else {
			      assert([request containsString:@"你好🌏"]);
			      assert([invocation emit:@"你好🌏"]);
			      response.text = @"你好🌏";
		      }
		      return response;
		    }
		     instructions:@"测试"
		            error:&error];
		assert(agent && !error);
		assert([agent addTool:@"echo"
		          description:@"Echo"
		               schema:@"{}"
		     requiresApproval:YES
		              handler:^LAIToolResult *(LAIToolCall *call,
		                  LAIInvocation *invocation) {
			        assert(
			            [call.identifier isEqualToString:@"编号"] &&
			            ![invocation stopped]);
			        tools++;
			        LAIToolResult *result = [LAIToolResult new];
			        result.text = @"你好🌏";
			        return result;
		              }
		                error:&error]);
		LAIRunHooks *hooks = [LAIRunHooks new];
		hooks.authorize = ^BOOL(NSString *, NSString *, BOOL required) {
		  return required;
		};
		hooks.onText = ^BOOL(NSString *text) {
		  return [text isEqualToString:@"你好🌏"];
		};
		NSDictionary *result = [agent run:@"开始"
		                          options:nil
		                            hooks:hooks
		                            error:&error];
		assert([result[@"status"] isEqualToString:@"completed"] &&
		    tools == 1);
		assert([result[@"text"] isEqualToString:@"你好🌏"]);
		@try {
			[saved stopped];
			assert(false);
		} @catch (NSException *exception) {
			(void)exception;
		}
		NSArray *messages = [agent messages:&error];
		assert(messages.count == 4);
		assert([messages[2][@"role"] isEqualToString:@"tool"]);
		LAICancellation *token = [LAICancellation new];
		[token cancel];
		assert(token.cancelled);
		LAIRunOptions *cancelled = [LAIRunOptions new];
		cancelled.cancellation = token;
		result = [agent run:@"skip"
		            options:cancelled
		              hooks:nil
		              error:&error];
		assert([result[@"status"] isEqualToString:@"cancelled"]);
		assert([agent reset:&error]);
		assert([agent messages:&error].count == 0);
		LAIRunOptions *options = [LAIRunOptions new];
		options.maxModelCalls = 0;
		result = [agent run:@"budget"
		            options:options
		              hooks:nil
		              error:&error];
		assert([result[@"status"] isEqualToString:@"budget_exceeded"]);
		assert([agent close:&error]);
		agent = [[LAIAgent alloc]
		    initWithModel:^LAIModelResponse *(
		        NSString *, LAIInvocation *) {
		      [NSException raise:@"Test" format:@"模型失败🌏"];
		      return nil;
		    }
		     instructions:@""
		            error:&error];
		result = [agent run:@"fail" options:nil hooks:nil error:&error];
		assert([result[@"status"] isEqualToString:@"model_error"]);
		assert([agent close:&error]);
		NSString *url = NSProcessInfo.processInfo
		                    .environment[@"LIBAI_TEST_PROVIDER_URL"];
		if (url) {
			agent = [[LAIAgent alloc] initWithProvider:@{
				@"protocol" : @"openai_chat",
				@"base_url" : url,
				@"model" : @"fixture"
			}
			                                    apiKey:@"temporary"
			                              instructions:@""
			                                     error:&error];
			assert(agent);
			result = [agent run:@"你好😀"
			            options:nil
			              hooks:nil
			              error:&error];
			assert([result[@"text"] isEqualToString:@"native😀"]);
			assert([agent close:&error]);
		}
		puts("Objective-C bridge tests passed");
	}
}
