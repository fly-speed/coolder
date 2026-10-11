#pragma once
#import "LAIAgent.h"

#include "libai.h"
NS_ASSUME_NONNULL_BEGIN

/** Private callback-local native handle; reset to NULL before returning. */
@interface LAIInvocation ()
/** Borrowed native boundary owned by the current synchronous callback. */
@property(nonatomic, assign) libai_invocation *_Nullable pointer;
/** Reject use of the borrowed handle from another thread. */
@property(nonatomic, strong) NSThread *thread;
@end

/** Agent state accessed only while its facade remains alive. */
@interface LAIAgent ()
/** Native owner of the C++ agent/session. */
@property(nonatomic, assign) libai_agent *_Nullable native;
/** Copied application model block. */
@property(nonatomic, copy) LAIModel model;
/** Named tool blocks retained until close. */
@property(nonatomic, strong) NSMutableDictionary<NSString *, LAITool> *tools;
/** Observer snapshot retained for the active run. */
@property(nonatomic, strong, nullable) LAIRunHooks *hooks;
/** Prevent close, mutation or simultaneous session runs. */
@property(nonatomic) BOOL busy;
@end

/** Check string encoding before passing borrowed bytes through the C ABI. */
const char *lai_utf8(NSString *text);
/** Convert bridge errors into Foundation errors without discarding the diagnostic. */
BOOL lai_error(NSError *_Nullable *_Nullable error, NSString *message);
/** Synchronous Objective-C model trampoline. */
void lai_model(void *, const char *, libai_response *, libai_invocation *);
/** Synchronous Objective-C application-tool trampoline. */
void lai_tool(void *, const char *, const char *, const char *,
    libai_tool_result *, libai_invocation *);
/** Forward streamed text through the active run's block. */
int lai_text(void *, const char *);
/** Apply the active run's per-call authorization block. */
int lai_authorize(void *, const char *, const char *, int);
/** Forward one lifecycle event to the active run's observer block. */
void lai_event(void *, const char *);

NS_ASSUME_NONNULL_END
