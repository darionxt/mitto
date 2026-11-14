// The window's caches: CacheStorage, as Blink's asks Chrome's browser for the
// caches of the document's storage key, which its Session keeps and every
// Page of the Session shares. Each promise settles from a task of its own once
// the Session has answered, and a match with a method other than GET settles
// without asking, unless it ignores the method.
//
// Upstream: third_party/blink/renderer/modules/cache_storage/cache_storage.cc
// Upstream: third_party/blink/renderer/modules/cache_storage/global_cache_storage.cc

#ifndef MITTO_DOM_CACHE_STORAGE_H_
#define MITTO_DOM_CACHE_STORAGE_H_

#include <string>
#include <string_view>

#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/cache_storage_messages.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-forward.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class CacheStorageHost;
class Window;

class CacheStorage final : public bindings::ScriptWrappable {
 public:
  explicit CacheStorage(Window* window);
  ~CacheStorage() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  v8::Local<v8::Promise> Match(const bindings::RequestOrUSVString& request,
                               const bindings::MultiCacheQueryOptions& options,
                               bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> Has(const std::u16string& cache_name,
                             bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> Open(const std::u16string& cache_name,
                              bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> Delete(const std::u16string& cache_name,
                                bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> Keys(bindings::ExceptionState& exception_state);

 private:
  // The document's host, or null once the window's context has gone, when
  // Blink's remote is unbound and each call rejects.
  CacheStorageHost* Host() const;
  // Asks the Session `call`, and settles the promise of `resolver` with what
  // `settle` makes of the answer.
  using Settle = void (*)(v8::Isolate* isolate,
                          v8::Local<v8::Context> context,
                          v8::Local<v8::Promise::Resolver> resolver,
                          CacheStorageHost& host,
                          Window& window,
                          const PageCacheStorageAnswer& answer);
  // `method` is the call's, whose name a rejection that this makes says.
  v8::Local<v8::Promise> Ask(PageCacheStorageCall call,
                             std::string_view method,
                             Settle settle);

  const cppgc::WeakMember<Window> window_;
};

}  // namespace mitto

#endif  // MITTO_DOM_CACHE_STORAGE_H_
