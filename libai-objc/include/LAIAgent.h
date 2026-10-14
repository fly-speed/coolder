#import <Foundation/Foundation.h>
#import "LAIExecution.h"

NS_ASSUME_NONNULL_BEGIN
@class LAICancellation;
@class LAIMcpClient;

/** A complete application-defined tool invocation; arguments are JSON text. */
@interface LAIToolCall : NSObject
/** Provider/runtime correlation identifier. */
@property(nonatomic, copy) NSString *identifier;
/** Registered application capability name. */
@property(nonatomic, copy) NSString *name;
/** Opaque JSON object to validate in the application's tool handler. */
@property(nonatomic, copy) NSString *arguments;
@end

/** Owned model output copied into native storage after callback return. */
@interface LAIModelResponse : NSObject
/** User-visible answer. */
@property(nonatomic, copy) NSString *text;
/** Optional protocol reasoning replay, separate from display text. */
@property(nonatomic, copy) NSString *reasoning;
/** Complete ordered invocation batch. */
@property(nonatomic, copy) NSArray<LAIToolCall *> *calls;
/** Reported provider input tokens. */
@property(nonatomic) NSUInteger inputTokens;
/** Reported provider output tokens. */
@property(nonatomic) NSUInteger outputTokens;
/** Empty on success; explicit failure diagnostics otherwise. */
@property(nonatomic, copy) NSString *error;
/** Whether the model failure can be retried. */
@property(nonatomic) BOOL retryable;
@end

/** Textual application tool result. */
@interface LAIToolResult : NSObject
/** Output copied by the native runtime. */
@property(nonatomic, copy) NSString *text;
/** Whether execution failed or was denied. */
@property(nonatomic) BOOL error;
@end

/** Callback-scoped cooperative execution boundary; expires after return. */
@interface LAIInvocation : NSObject
/** Check cancellation or the inherited run deadline. */
- (BOOL)stopped;
/** -1 unlimited, 0 expired. Never turn 0 into an unlimited child timeout. */
- (int64_t)remainingMs;
/** Own a copy of the shared stop state; nil/error if absent or allocation fails. */
- (nullable LAICancellation *)retainCancellation:(NSError *_Nullable *_Nullable)error;
/** Copy the durable effect key while inside the callback; empty if disabled. */
- (NSString *)idempotencyKey;
/** Forward a model text delta; false requests the model to stop. */
- (BOOL)emit:(NSString *)text;
@end

/** Independent atomic stop state; usable before a run or from another thread. */
@interface LAICancellation : NSObject
/** Request cooperative cancellation without resetting the token. */
- (void)cancel;
/** Inspect whether cancellation was requested. */
@property(nonatomic, readonly) BOOL cancelled;
@end

/** Stable run limits initialized to the C++ SDK defaults. */
@interface LAIRunOptions : NSObject
/** Logical model-attempt ceiling. */
@property(nonatomic) NSUInteger maxModelCalls;
/** Executed tool-handler ceiling. */
@property(nonatomic) NSUInteger maxToolCalls;
/** Total reported output token ceiling. */
@property(nonatomic) NSUInteger maxOutputTokens;
/** Per-request output token ceiling. */
@property(nonatomic) NSUInteger maxTokensPerCall;
/** Request/schema byte ceiling. */
@property(nonatomic) NSUInteger maxContextBytes;
/** Per-tool result byte ceiling. */
@property(nonatomic) NSUInteger maxToolResultBytes;
/** Recoverable model retries per logical turn. */
@property(nonatomic) NSUInteger maxModelRetries;
/** Cooperative deadline duration in milliseconds. */
@property(nonatomic) int64_t timeoutMs;
/** Optional shared stop token; nil allocates a per-run token. */
@property(nonatomic, strong, nullable) LAICancellation *cancellation;
@end

/** Application model block; no native provider SDK is required on iOS. */
typedef LAIModelResponse *_Nonnull (^LAIModel)(NSString *, LAIInvocation *);
/** Application tool block; validate JSON arguments before performing actions. */
typedef LAIToolResult *_Nonnull (^LAITool)(LAIToolCall *, LAIInvocation *);
/** Return false to stop forwarding model text. */
typedef BOOL (^LAITextSink)(NSString *);
/** Authorization policy for a concrete tool name and argument object. */
typedef BOOL (^LAIAuthorization)(NSString *, NSString *, BOOL);
/** Observe one native lifecycle event serialized as JSON. */
typedef void (^LAIEventSink)(NSString *);

/** Optional application observers and permissions for a synchronous run. */
@interface LAIRunHooks : NSObject
/** Streaming text observer executed on the run thread. */
@property(nonatomic, copy, nullable) LAITextSink onText;
/** Per-call permissions; missing callback denies approval-required tools. */
@property(nonatomic, copy, nullable) LAIAuthorization authorize;
/** Lifecycle-event observer executed on the run thread. */
@property(nonatomic, copy, nullable) LAIEventSink onEvent;
@end

/** Owning Objective-C facade over a persistent native agent session. */
@interface LAIAgent : NSObject
/** Require an application model rather than constructing an unusable facade. */
- (instancetype)init NS_UNAVAILABLE;
/** Retain the model block and copy stable instructions. */
- (nullable instancetype)initWithModel:(LAIModel)model
                          instructions:(NSString *)instructions
                                 error:(NSError *_Nullable *_Nullable)error;
/** Construct the built-in desktop Provider using endpoint options and an in-memory key. */
- (nullable instancetype)initWithProvider:(NSDictionary<NSString *, id> *)config
                                   apiKey:(NSString *)apiKey
                             instructions:(NSString *)instructions
                                    error:(NSError *_Nullable *_Nullable)error;
/** Copy the idle transcript with complete tool calls and reasoning. */
- (nullable NSArray<NSDictionary<NSString *, id> *> *)messages:
    (NSError *_Nullable *_Nullable)error;
/** Configure idle-agent capabilities from a versioned profile dictionary. */
- (BOOL)configureModel:(NSDictionary<NSString *, id> *)profile
                 error:(NSError *_Nullable *_Nullable)error;
/** Host-validate final JSON; nil disables validation. Feedback range 0..8. */
- (BOOL)configureOutput:(nullable NSDictionary<NSString *, id> *)schema
               repairs:(NSUInteger)repairs error:(NSError *_Nullable *_Nullable)error;
/** Exact JSON schema text, including schemas recovered from another binding. nil disables. */
- (BOOL)configureOutputJSON:(nullable NSString *)schemaJSON
                   repairs:(NSUInteger)repairs error:(NSError *_Nullable *_Nullable)error;
/** Register an idle agent's application capability and handler. */
- (BOOL)addTool:(NSString *)name
         description:(NSString *)description
              schema:(NSString *)schema
    requiresApproval:(BOOL)approval
             handler:(LAITool)handler
               error:(NSError *_Nullable *_Nullable)error;
/** Configure a durable task while idle; nil disables and the Agent retains the store. */
- (BOOL)setExecutionStore:(nullable id<LAIExecutionStore>)store scope:(NSString *)scope
                   error:(NSError *_Nullable *_Nullable)error;
/** Bind MCP with host policy. Schema fields may be dictionaries (sorted encoding)
 * or exact JSON strings; defaults approval=true, side_effect_free=false. */
- (BOOL)addMcpTool:(LAIMcpClient *)client remoteName:(NSString *)remote localName:(NSString *)local
    policy:(NSDictionary *)policy error:(NSError *_Nullable *_Nullable)error;
/** Register with a host execution version; change it when handler semantics change. */
- (BOOL)addTool:(NSString *)name description:(NSString *)description schema:(NSString *)schema
    requiresApproval:(BOOL)approval executionRevision:(NSString *)revision handler:(LAITool)handler
               error:(NSError *_Nullable *_Nullable)error;
/** Trusted host assertion; only handlers without external effects may retry automatically. */
- (BOOL)addTool:(NSString *)name description:(NSString *)description schema:(NSString *)schema
    requiresApproval:(BOOL)approval executionRevision:(NSString *)revision sideEffectFree:(BOOL)sideEffectFree
             handler:(LAITool)handler error:(NSError *_Nullable *_Nullable)error;
/** Execute off the UI thread; return status/text/error and accounting. */
- (nullable NSDictionary<NSString *, id> *)run:(NSString *)input
                                       options:(nullable LAIRunOptions *)options
                                         hooks:(nullable LAIRunHooks *)hooks
                                         error:(NSError *_Nullable *_Nullable)
                                                   error;
/** May be called concurrently with run to request cooperative cancellation. */
- (void)cancel;
/** Clear the idle agent's transcript while retaining its registered tools. */
- (BOOL)reset:(NSError *_Nullable *_Nullable)error;
/** Release native state while idle; dealloc also closes an idle facade. */
- (BOOL)close:(NSError *_Nullable *_Nullable)error;
@end
NS_ASSUME_NONNULL_END
