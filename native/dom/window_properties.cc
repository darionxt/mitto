#include "mitto/dom/window_properties.h"

#include "mitto/dom/document.h"
#include "mitto/dom/element.h"
#include "mitto/dom/html_collection.h"
#include "mitto/dom/window.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-object.h"

namespace mitto {

WindowProperties::WindowProperties() = default;
WindowProperties::~WindowProperties() = default;

v8::Local<v8::Value> WindowProperties::AnonymousNamedGetter(
    const std::u16string& name) {
  // Window is the only class that inherits from this one. The embedder sets
  // properties of a new context's window before it has its document.
  Window* const window = static_cast<Window*>(this);
  if (!window->HasDocument()) {
    return v8::Local<v8::Value>();
  }
  Document& document = window->GetDocument();
  if (!document.IsHTMLDocument()) {
    return v8::Local<v8::Value>();
  }
  const bool has_named_item = document.HasNamedItem(name);
  const bool has_id_item = document.HasElementWithId(name);
  if (!has_named_item && !has_id_item) {
    return v8::Local<v8::Value>();
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (!has_named_item && !document.ContainsMultipleElementsWithId(name)) {
    if (Element* const element = document.GetElementById(name)) {
      return element->ToV8(isolate);
    }
    return v8::Local<v8::Value>();
  }
  HTMLCollection* const items = document.WindowNamedItems(name);
  const uint32_t length = items->Length();
  if (length == 0) {
    return v8::Local<v8::Value>();
  }
  if (length == 1) {
    return items->Item(0)->ToV8(isolate);
  }
  return items->ToV8(isolate);
}

}  // namespace mitto
