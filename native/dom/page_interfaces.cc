#include "mitto/dom/page_interfaces.h"

#include <utility>

#include "base/check.h"
#include "mitto/bindings/interface_objects.h"
#include "mitto/bindings/per_context_data.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/v8_dedicated_worker_global_scope.h"
#include "mitto/bindings/v8_window.h"
#include "mitto/dom/dedicated_worker_global_scope.h"
#include "mitto/dom/document.h"
#include "mitto/dom/dom_exception.h"
#include "mitto/dom/event_handlers.h"
#include "mitto/dom/html_tree_builder.h"
#include "mitto/dom/objects_behind.h"
#include "mitto/dom/window.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-template.h"

// Upstream: third_party/blink/renderer/bindings/core/v8/local_window_proxy.cc

namespace mitto {

v8::Local<v8::Context> NewPageContext(v8::Isolate* isolate,
                                      const ContextInit& init,
                                      std::u16string_view markup,
                                      DidCreateScriptContext did_create) {
  return NewPageContext(isolate, init, markup, v8::Local<v8::Object>(),
                        did_create);
}

v8::Local<v8::Context> NewPageContext(v8::Isolate* isolate,
                                      const ContextInit& init,
                                      std::u16string_view markup,
                                      v8::Local<v8::Object> global_proxy,
                                      DidCreateScriptContext did_create) {
  bindings::PerIsolateData* const data =
      bindings::PerIsolateData::From(isolate);
  data->SetDOMExceptionHooks(DOMException::BindingsHooks());
  bindings::PerIsolateData::ObjectHooks hooks = ObjectsBehindHooks();
  AddEventHandlerHooks(hooks);
  data->SetObjectHooks(hooks);
  // A new document reports each Known Gap it reaches again.
  data->StartDocument();
  const v8::Local<v8::ObjectTemplate> global_template =
      data->InterfaceTemplate(isolate, &bindings::V8Window::kWrapperTypeInfo)
          ->InstanceTemplate();
  const v8::Local<v8::Context> context = v8::Context::New(
      isolate, nullptr, global_template,
      global_proxy.IsEmpty() ? v8::MaybeLocal<v8::Value>()
                             : v8::MaybeLocal<v8::Value>(global_proxy));
  const v8::Context::Scope context_scope(context);
  Window* const window = Window::Create();
  window->AssociateWithContext(isolate, context);
  window->SetTimeOrigin(init.time_origin);
  window->SetSecureContext(init.secure_context);
  bindings::SetSecureContext(context, init.secure_context);
  window->SetCrossOriginIsolatedCapability(init.cross_origin_isolated);
  bindings::SetCrossOriginIsolated(context, init.cross_origin_isolated);
  // LocalWindowProxy::Initialize(): the inspector and the embedder hear of the
  // context before its conditional features go in.
  did_create(context);
  bindings::InstallInterfaceObjects(context);
  // LocalWindowProxy::InstallConditionalFeatures(): then V8 extends the
  // context with what its embedder allows it, SharedArrayBuffer for a
  // cross-origin isolated window.
  isolate->InstallConditionalFeatures(context);
  // The document of about:blank is what the parser makes of no markup, as
  // the window's document, whose parser has the scripting flag.
  Document* const document = Document::CreateHTML();
  window->SetDocument(document);
  ParseHtmlDocument(*document, markup);
  return context;
}

// Upstream: third_party/blink/renderer/bindings/core/v8/worker_or_worklet_script_controller.cc
v8::Local<v8::Context> NewWorkerContext(v8::Isolate* isolate,
                                        const ContextInit& init,
                                        std::u16string name,
                                        DidCreateScriptContext did_create) {
  bindings::PerIsolateData* const data =
      bindings::PerIsolateData::From(isolate);
  CHECK(!data->is_window());
  data->SetDOMExceptionHooks(DOMException::BindingsHooks());
  bindings::PerIsolateData::ObjectHooks hooks = ObjectsBehindHooks();
  AddEventHandlerHooks(hooks);
  data->SetObjectHooks(hooks);
  data->StartDocument();
  const v8::Local<v8::ObjectTemplate> global_template =
      data->InterfaceTemplate(
              isolate,
              &bindings::V8DedicatedWorkerGlobalScope::kWrapperTypeInfo)
          ->InstanceTemplate();
  const v8::Local<v8::Context> context =
      v8::Context::New(isolate, nullptr, global_template);
  const v8::Context::Scope context_scope(context);
  DedicatedWorkerGlobalScope* const scope =
      DedicatedWorkerGlobalScope::Create(std::move(name));
  scope->AssociateWithContext(isolate, context);
  scope->SetTimeOrigin(init.time_origin);
  scope->SetSecureContext(init.secure_context);
  bindings::SetSecureContext(context, init.secure_context);
  scope->SetCrossOriginIsolatedCapability(init.cross_origin_isolated);
  bindings::SetCrossOriginIsolated(context, init.cross_origin_isolated);
  // Initialize() names the context to the debugger before
  // PrepareForEvaluation() makes the interface object of the global, which
  // installs what the context decides, and then has V8 extend the context.
  did_create(context);
  bindings::InstallWorkerInterfaceObjects(context);
  isolate->InstallConditionalFeatures(context);
  return context;
}

}  // namespace mitto
