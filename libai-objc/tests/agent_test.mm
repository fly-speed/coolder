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
		assert([agent reset:&error]);
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
		puts("Objective-C bridge tests passed");
	}
}
