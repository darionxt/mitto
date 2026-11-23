// What the bindings keep for each context of a Page's isolate, as Blink keeps
// it in V8PerContextData and ScriptState: whether the context is a secure
// context, and in which context a binding call makes the wrappers it returns.
//
// A Page has a context for each of its frames' windows, so what differs from
// one document to another is kept with its context, not with the isolate.
//
// Upstream: third_party/blink/renderer/platform/bindings/v8_per_context_data.h
// Upstream: third_party/blink/renderer/platform/bindings/script_state.h

#ifndef MITTO_BINDINGS_PER_CONTEXT_DATA_H_
#define MITTO_BINDINGS_PER_CONTEXT_DATA_H_

#include "base/memory/raw_ptr.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-object.h"

namespace mitto::bindings {

// Says whether `context` is a secure context, which decides whether it gets
// the members that Web IDL's [SecureContext] puts behind one. Each document's
// origin decides it for the context made for that document, and a dedicated
// worker's creator for the worker's. Set before the context installs what it
// decides.
void SetSecureContext(v8::Local<v8::Context> context, bool secure);

// Whether `context` is a secure context. One that was never told, as the Page
// program's command line makes, is one, as a file: page is.
bool IsSecureContext(v8::Local<v8::Context> context);

// Says whether `context` is cross-origin isolated, which decides whether it
// gets the members that [CrossOriginIsolated] and
// [CrossOriginIsolatedOrRuntimeEnabled] put behind its capability. Set before
// the context installs what it decides.
void SetCrossOriginIsolated(v8::Local<v8::Context> context, bool isolated);

// Whether `context` is cross-origin isolated. One that was never told is not.
bool IsCrossOriginIsolated(v8::Local<v8::Context> context);

// The relevant realm of a binding call's receiver, where the call makes the
// wrappers that it returns, as Blink's bindings convert a return value with
// the receiver's ScriptState (ScriptState::ForRelevantRealm()). While one is
// in scope, and the isolate's current context is still the one it was made
// in, ScriptWrappable::ToV8() makes a new wrapper in the receiver's context.
// Outside one, or once a script of another context runs inside it, wrappers
// are made in the current context, as for a constructor's or a static
// operation's call, which have no receiver.
//
// It lives on the stack, though PerIsolateData points at the innermost one.
class ReceiverRealmScope final {
 public:
  ReceiverRealmScope(v8::Isolate* isolate, v8::Local<v8::Object> receiver);

  ReceiverRealmScope(const ReceiverRealmScope&) = delete;
  ReceiverRealmScope& operator=(const ReceiverRealmScope&) = delete;

  ~ReceiverRealmScope();

  // The context in which ToV8() makes a new wrapper now: the receiver's of
  // the innermost scope while it holds, and otherwise the current one.
  static v8::Local<v8::Context> CreationContext(v8::Isolate* isolate);

 private:
  const raw_ptr<v8::Isolate> isolate_;
  const raw_ptr<ReceiverRealmScope> outer_;
  // The isolate's current context when the call began.
  v8::Local<v8::Context> current_;
  // The receiver's creation context, or an empty one if it has none.
  v8::Local<v8::Context> receiver_;
};

}  // namespace mitto::bindings

#endif  // MITTO_BINDINGS_PER_CONTEXT_DATA_H_
