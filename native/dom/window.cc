#include "mitto/dom/window.h"

#include "base/check.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_window.h"
#include "mitto/dom/document.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

// static
Window* Window::Create() {
  return bindings::MakeGarbageCollected<Window>();
}

// static
Window& Window::Current() {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  Window* const window = bindings::V8Window::ToWrappable(
      isolate, isolate->GetCurrentContext()->Global());
  CHECK(window);
  return *window;
}

Window::Window() = default;
Window::~Window() = default;

const bindings::WrapperTypeInfo* Window::GetWrapperTypeInfo() const {
  return &bindings::V8Window::kWrapperTypeInfo;
}

void Window::Trace(cppgc::Visitor* visitor) const {
  WindowProperties::Trace(visitor);
  visitor->Trace(document_);
}

void Window::SetDocument(mitto::Document* document) {
  if (document_) {
    document_->SetWindow(nullptr);
  }
  document_ = document;
  document_->SetWindow(this);
}

void Window::AssociateWithContext(v8::Isolate* isolate,
                                  v8::Local<v8::Context> context) {
  AssociateWithGlobalProxy(isolate, context->Global());
}

}  // namespace mitto
