// The HTML Standard's Window, a Page's global object, with `window`, `self`,
// `document`, `performance`, the timers, queueMicrotask() and fetch(); its
// other members are Known Gaps.
//
// A context's global object and its global proxy both wrap the Window, as in
// Blink, so that a call whose receiver is either finds it. The window's
// wrapper is the global proxy, which `window`, `self` and globalThis are.
//
// Upstream: third_party/blink/renderer/core/frame/dom_window.h
// Upstream: third_party/blink/renderer/core/frame/local_dom_window.h
// Upstream: third_party/blink/renderer/bindings/core/v8/local_window_proxy.cc

#ifndef MITTO_DOM_WINDOW_H_
#define MITTO_DOM_WINDOW_H_

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/dom/window_properties.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-local-handle.h"

namespace mitto {

class DOMTimerCoordinator;
class Document;
class Fetcher;
class Performance;

class Window final : public WindowProperties {
 public:
  static Window* Create();
  // The Window of the isolate's current context.
  static Window& Current();

  Window();
  ~Window() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  Window* WindowForBinding() { return this; }
  Window* Self() { return this; }
  mitto::Document* Document() const { return document_.Get(); }
  mitto::Performance* Performance();
  int32_t SetTimeout(v8::Local<v8::Function> handler,
                     int32_t timeout,
                     const std::vector<v8::Local<v8::Value>>& arguments);
  int32_t SetTimeout(const std::u16string& handler,
                     int32_t timeout,
                     const std::vector<v8::Local<v8::Value>>& arguments,
                     bindings::ExceptionState& exception_state);
  void ClearTimeout(int32_t handle);
  int32_t SetInterval(v8::Local<v8::Function> handler,
                      int32_t timeout,
                      const std::vector<v8::Local<v8::Value>>& arguments);
  int32_t SetInterval(const std::u16string& handler,
                      int32_t timeout,
                      const std::vector<v8::Local<v8::Value>>& arguments,
                      bindings::ExceptionState& exception_state);
  void ClearInterval(int32_t handle);
  void QueueMicrotask(v8::Local<v8::Function> callback);
  v8::Local<v8::Promise> Fetch(const bindings::RequestOrUSVString& input,
                               const bindings::RequestInit& init,
                               bindings::ExceptionState& exception_state);

  // What the Page uses.
  mitto::Document& GetDocument() const { return *document_; }
  // Makes `document` the window's, and the window its document's.
  void SetDocument(mitto::Document* document);
  // Makes the global proxy and the global object of `context`, which was made
  // from Window's template, this window's wrappers, as Blink's
  // LocalWindowProxy::SetupWindowPrototypeChain() does.
  void AssociateWithContext(v8::Isolate* isolate,
                            v8::Local<v8::Context> context);
  // The timers of the window's context, which the Page program keeps, and
  // which go with it. Without them, as once the context is gone, a timer set
  // is never run, and its ID is 0, as Chrome gives in a destroyed context.
  void SetTimers(DOMTimerCoordinator* timers) { timers_ = timers; }
  // How many timers are set.
  size_t TimerCount() const;
  // What fetch() asks the Session through. Without one, it fails.
  void SetFetcher(Fetcher* fetcher) { fetcher_ = fetcher; }

 private:
  cppgc::Member<mitto::Document> document_;
  cppgc::Member<mitto::Performance> performance_;
  raw_ptr<DOMTimerCoordinator> timers_ = nullptr;
  raw_ptr<Fetcher> fetcher_ = nullptr;
};

}  // namespace mitto

#endif  // MITTO_DOM_WINDOW_H_
