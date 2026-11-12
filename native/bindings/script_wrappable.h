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

#include <type_traits>
#include <utility>

#include "base/check_op.h"
#include "base/memory/raw_ptr_exclusion.h"
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

  // Makes a context's global proxy this object's wrapper, and has both the
  // proxy and the global object behind it wrap this object, as Blink's
  // V8DOMWrapper::SetNativeInfoForGlobal() does for a window.
  void AssociateWithGlobalProxy(v8::Isolate* isolate,
                                v8::Local<v8::Object> global_proxy);

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

// An object of class `T` whose wrapper is of `info`, an interface without a
// class that inherits from T's, as a Known Gap makes one through
// WrapperTypeInfo::create_for_known_gap: T's members work on it, and its own
// stay Known Gaps.
template <typename T>
class OfInterface final : public T {
 public:
  template <typename... Args>
  explicit OfInterface(const WrapperTypeInfo& info, Args&&... args)
      : T(std::forward<Args>(args)...), info_(&info) {}

  const WrapperTypeInfo* GetWrapperTypeInfo() const override { return info_; }

 private:
  // RAW_PTR_EXCLUSION: #global-scope, as WrapperTypeInfo's own.
  RAW_PTR_EXCLUSION const WrapperTypeInfo* const info_;
};

// What a class's CreateForKnownGap() gives for `info`: an object of T, made
// with `args`, whose wrapper is T's own or, for an interface without a class
// that inherits from T's, of that interface, for which T cannot be final.
template <typename T, typename... Args>
ScriptWrappable* MakeForKnownGap(const WrapperTypeInfo& info,
                                 const WrapperTypeInfo& own,
                                 Args&&... args) {
  if constexpr (std::is_final_v<T>) {
    CHECK_EQ(&info, &own);
    return MakeGarbageCollected<T>(std::forward<Args>(args)...);
  } else {
    if (&info == &own) {
      return MakeGarbageCollected<T>(std::forward<Args>(args)...);
    }
    return MakeGarbageCollected<OfInterface<T>>(info,
                                                std::forward<Args>(args)...);
  }
}

}  // namespace mitto::bindings

#endif  // MITTO_BINDINGS_SCRIPT_WRAPPABLE_H_
