#include "mitto/dom/history.h"

#include "mitto/bindings/v8_history.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

// static
History* History::Create() {
  return bindings::MakeGarbageCollected<History>();
}

// static
bindings::ScriptWrappable* History::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<History>(
      info, bindings::V8History::kWrapperTypeInfo);
}

History::History() = default;
History::~History() = default;

const bindings::WrapperTypeInfo* History::GetWrapperTypeInfo() const {
  return &bindings::V8History::kWrapperTypeInfo;
}

v8::Local<v8::Value> History::State(
    bindings::ExceptionState& exception_state) const {
  return v8::Null(v8::Isolate::GetCurrent());
}

}  // namespace mitto
