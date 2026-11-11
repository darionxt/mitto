#include "mitto/dom/page_interfaces.h"

#include "mitto/bindings/interface_objects.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/dom/dom_exception.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

void InstallPageInterfaces(v8::Local<v8::Context> context) {
  bindings::PerIsolateData::From(v8::Isolate::GetCurrent())
      ->SetDOMExceptionHooks(DOMException::BindingsHooks());
  bindings::InstallInterfaceObjects(context);
}

}  // namespace mitto
