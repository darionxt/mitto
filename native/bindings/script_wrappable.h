// The base of every C++ object that a page can reach through the bindings.
//
// Such objects live on V8's C++ heap (cppgc), as Blink's do on Oilpan, which is
// built on it. A wrapper, the JavaScript object a page sees, holds its C++
// object through V8's wrapping, and the C++ object holds its wrapper through a
// traced reference, so that V8's collector follows both ways and can collect a
// cycle between a page's objects and the Runtime's.
//
// Upstream: third_party/blink/renderer/platform/bindings/script_wrappable.h

#ifndef MITTO_BINDINGS_SCRIPT_WRAPPABLE_H_
#define MITTO_BINDINGS_SCRIPT_WRAPPABLE_H_

#include <utility>

#include "mitto/bindings/wrapper_type_info.h"
#include "v8/include/cppgc/allocation.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-cppgc.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-traced-handle.h"

namespace mitto::bindings {

class ScriptWrappable : public v8::Object::Wrappable {
 public:
  ScriptWrappable(const ScriptWrappable&) = delete;
  ScriptWrappable& operator=(const ScriptWrappable&) = delete;

  // The interface whose wrapper this object gets, its most derived one.
  const WrapperTypeInfo* GetWrapperTypeInfo() const override = 0;

  void Trace(cppgc::Visitor* visitor) const override;

  // Returns the wrapper in the isolate's current context, and makes it if this
  // object has none yet.
  v8::Local<v8::Object> ToV8(v8::Isolate* isolate);

  // Makes `wrapper`, which a constructor of the interface got as its receiver,
  // this object's wrapper.
  void AssociateWithWrapper(v8::Isolate* isolate,
                            v8::Local<v8::Object> wrapper);

 protected:
  ScriptWrappable() = default;

 private:
  v8::TracedReference<v8::Object> wrapper_;
};

// Allocates a T on the C++ heap of the current isolate.
template <typename T, typename... Args>
T* MakeGarbageCollected(Args&&... args) {
  return cppgc::MakeGarbageCollected<T>(
      v8::Isolate::GetCurrent()->GetCppHeap()->GetAllocationHandle(),
      std::forward<Args>(args)...);
}

}  // namespace mitto::bindings

#endif  // MITTO_BINDINGS_SCRIPT_WRAPPABLE_H_
