#include "mitto/dom/document_named_properties.h"

#include <string>
#include <tuple>

#include "base/memory/raw_ptr.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_document.h"
#include "mitto/dom/document.h"
#include "mitto/dom/element.h"
#include "mitto/dom/html_collection.h"
#include "mitto/dom/window.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-function-callback.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

namespace {

// Blink's GetNamedProperty(): the one element of the name, or the collection
// of them. An iframe gives its window, which comes with frames.
v8::Local<v8::Value> GetNamedProperty(v8::Isolate* isolate,
                                      Document& document,
                                      const std::u16string& name) {
  if (!document.HasNamedItem(name)) {
    return v8::Local<v8::Value>();
  }
  HTMLCollection* const items = document.DocumentNamedItems(name);
  const uint32_t length = items->Length();
  if (length == 0) {
    return v8::Local<v8::Value>();
  }
  if (length == 1) {
    return items->Item(0)->ToV8(isolate);
  }
  return items->ToV8(isolate);
}

// Getter(): the named property, and otherwise what the prototype chain has of
// the name, such as Document's title once the img named title that shadowed
// it has gone.
void Getter(v8::Local<v8::Name> property,
            const v8::PropertyCallbackInfo<v8::Value>& info) {
  if (!property->IsString()) {
    return;
  }
  v8::Isolate* const isolate = info.GetIsolate();
  std::u16string name;
  if (!bindings::FromV8String(isolate, property, &name)) {
    return;
  }
  Document* const document = bindings::ToWrappableOf<Document>(
      isolate, bindings::V8Document::kWrapperTypeInfo, info.Holder());
  if (!document) {
    return;
  }
  const v8::Local<v8::Value> value = GetNamedProperty(isolate, *document, name);
  if (!value.IsEmpty()) {
    info.GetReturnValue().Set(value);
    return;
  }
  v8::Local<v8::Value> inherited;
  if (info.Holder()
          ->GetRealNamedPropertyInPrototypeChain(isolate->GetCurrentContext(),
                                                 property.As<v8::String>())
          .ToLocal(&inherited)) {
    info.GetReturnValue().Set(inherited);
  }
}

// EmptySetter(): keeps the property an accessor when a page sets it.
void EmptySetter(v8::Local<v8::Name> property,
                 v8::Local<v8::Value> value,
                 const v8::PropertyCallbackInfo<v8::Boolean>& info) {}

// The wrapper of `document` in its window's context, entered, if it is its
// window's document.
class WindowDocumentScope {
 public:
  explicit WindowDocumentScope(Document& document)
      : isolate_(v8::Isolate::GetCurrent()), handle_scope_(isolate_.get()) {
    Window* const window = document.DefaultView();
    if (!window) {
      return;
    }
    // A window's wrapper is its context's global proxy from the start, until
    // its frame's next window takes it, whose context is not this window's.
    v8::Local<v8::Context> context;
    if (!window->ToV8(isolate())->GetCreationContext(isolate()).ToLocal(
            &context) ||
        Window::From(isolate(), context) != window) {
      return;
    }
    context_ = context;
    context_->Enter();
    wrapper_ = document.ToV8(isolate());
  }
  ~WindowDocumentScope() {
    if (!context_.IsEmpty()) {
      context_->Exit();
    }
  }
  WindowDocumentScope(const WindowDocumentScope&) = delete;
  WindowDocumentScope& operator=(const WindowDocumentScope&) = delete;

  v8::Isolate* isolate() const { return isolate_.get(); }
  v8::Local<v8::Context> context() const { return context_; }
  // Empty for a document that is not its window's.
  v8::Local<v8::Object> wrapper() const { return wrapper_; }

 private:
  const raw_ptr<v8::Isolate> isolate_;
  const v8::HandleScope handle_scope_;
  v8::Local<v8::Context> context_;
  v8::Local<v8::Object> wrapper_;
};

}  // namespace

void NamedItemAdded(Document& document, const std::u16string& name) {
  const WindowDocumentScope scope(document);
  if (scope.wrapper().IsEmpty()) {
    return;
  }
  // SetNativeDataProperty() throws for a property that cannot be
  // configured, which wins, as own properties come before named properties.
  const v8::TryCatch try_catch(scope.isolate());
  std::ignore = scope.wrapper()->SetNativeDataProperty(
      scope.context(), bindings::ToV8String(scope.isolate(), name), &Getter,
      &EmptySetter);
}

void NamedItemRemoved(Document& document, const std::u16string& name) {
  if (document.HasNamedItem(name)) {
    return;
  }
  const WindowDocumentScope scope(document);
  if (scope.wrapper().IsEmpty()) {
    return;
  }
  std::ignore = scope.wrapper()->Delete(
      scope.context(), bindings::ToV8String(scope.isolate(), name));
}

void AddNamedItemProperties(Document& document) {
  for (const std::u16string& name : document.NamedItems()) {
    NamedItemAdded(document, name);
  }
}

}  // namespace mitto
