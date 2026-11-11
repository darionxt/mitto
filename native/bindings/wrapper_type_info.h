// What the bindings know of each interface that has a C++ class: its name, its
// parent, the tags of its objects and how to make its templates. The bindings
// generator writes one for each such interface, as Blink's writes its
// WrapperTypeInfo.
//
// Upstream: third_party/blink/renderer/platform/bindings/wrapper_type_info.h

#ifndef MITTO_BINDINGS_WRAPPER_TYPE_INFO_H_
#define MITTO_BINDINGS_WRAPPER_TYPE_INFO_H_

#include "base/memory/raw_ptr_exclusion.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-sandbox.h"

namespace mitto::bindings {

struct WrapperTypeInfo : v8::Object::WrapperTypeInfo {
  using InstallInterfaceTemplateFunction =
      void (*)(v8::Isolate*, v8::Local<v8::FunctionTemplate>);

  const char* interface_name;
  // The interface it inherits from, or null.
  // RAW_PTR_EXCLUSION: #global-scope, as in Blink's WrapperTypeInfo.
  RAW_PTR_EXCLUSION const WrapperTypeInfo* parent;
  // The tag of the interface's own objects. Those of every interface that
  // inherits from it lie after it, up to `max_subclass_tag`, as Blink's Web
  // IDL database numbers them.
  v8::CppHeapPointerTag tag;
  v8::CppHeapPointerTag max_subclass_tag;
  // Installs the interface's members on its template, which inherits from its
  // parent's.
  InstallInterfaceTemplateFunction install_interface_template;
};

}  // namespace mitto::bindings

#endif  // MITTO_BINDINGS_WRAPPER_TYPE_INFO_H_
