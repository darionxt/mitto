#include "mitto/dom/screen_orientation.h"

#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_screen_orientation.h"
#include "mitto/dom/script_values.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"

namespace mitto {
namespace {

// GetOrientationTypeForDesktop(): a display that is at least as high as it is
// wide is in portrait. A Machine holds no rotation, so its display is in its
// primary orientation, at the angle 0.
std::u16string OrientationType(int width, int height) {
  return height >= width ? u"portrait-primary" : u"landscape-primary";
}

}  // namespace

// static
ScreenOrientation* ScreenOrientation::Create(int width, int height) {
  return bindings::MakeGarbageCollected<ScreenOrientation>(width, height);
}

ScreenOrientation::ScreenOrientation(int width, int height)
    : type_(OrientationType(width, height)) {}

ScreenOrientation::~ScreenOrientation() = default;

const bindings::WrapperTypeInfo* ScreenOrientation::GetWrapperTypeInfo() const {
  return &bindings::V8ScreenOrientation::kWrapperTypeInfo;
}

uint16_t ScreenOrientation::Angle() const {
  return 0;
}

std::u16string ScreenOrientation::Type() const {
  return type_;
}

v8::Local<v8::Promise> ScreenOrientation::Lock(
    const std::u16string& orientation,
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(isolate->GetCurrentContext())
           .ToLocal(&resolver)) {
    return v8::Local<v8::Promise>();
  }
  // Chrome on Windows has no ScreenOrientationDelegate, so the browser answers
  // that it cannot lock, and the renderer rejects once the answer comes.
  RejectWithDOMExceptionFromTask(
      isolate, resolver, "NotSupportedError",
      "screen.orientation.lock() is not available on this device.");
  return resolver->GetPromise();
}

void ScreenOrientation::Unlock() {
  // Nothing is locked.
}

}  // namespace mitto
