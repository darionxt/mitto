#include "mitto/dom/page_interfaces.h"

#include "mitto/bindings/interface_objects.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/v8_window.h"
#include "mitto/dom/document.h"
#include "mitto/dom/dom_exception.h"
#include "mitto/dom/html_tree_builder.h"
#include "mitto/dom/window.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-template.h"

// Upstream: third_party/blink/renderer/bindings/core/v8/local_window_proxy.cc

namespace mitto {

v8::Local<v8::Context> NewPageContext(v8::Isolate* isolate,
                                      std::u16string_view markup,
                                      DidCreateScriptContext did_create) {
  bindings::PerIsolateData* const data =
      bindings::PerIsolateData::From(isolate);
  data->SetDOMExceptionHooks(DOMException::BindingsHooks());
  const v8::Local<v8::ObjectTemplate> global_template =
      data->InterfaceTemplate(isolate, &bindings::V8Window::kWrapperTypeInfo)
          ->InstanceTemplate();
  const v8::Local<v8::Context> context =
      v8::Context::New(isolate, nullptr, global_template);
  const v8::Context::Scope context_scope(context);
  Window* const window = Window::Create();
  window->AssociateWithContext(isolate, context);
  bindings::InstallInterfaceObjects(context);
  // LocalWindowProxy::Initialize(): the inspector and the embedder hear of the
  // context before its conditional features go in.
  did_create(context);
  bindings::InstallConditionalFeatures(context);
  // The document of about:blank is what the parser makes of no markup.
  Document* const document = Document::CreateHTML();
  ParseHtmlDocument(*document, markup);
  window->SetDocument(document);
  return context;
}

}  // namespace mitto
