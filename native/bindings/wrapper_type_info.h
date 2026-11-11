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
  // Installs what a context decides on that context's interface object,
  // prototype and, for a global interface, the global object, which is empty
  // for any other.
  using InstallContextDependentPropertiesFunction =
      void (*)(v8::Local<v8::Context> context,
               v8::Local<v8::Object> interface_object,
               v8::Local<v8::Object> prototype_object,
               v8::Local<v8::Object> instance_object);

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
  // The interface whose interface object is the prototype of this one's: its
  // parent, or the parent's, past those with no interface object, as
  // WindowProperties has none.
  // RAW_PTR_EXCLUSION: #global-scope, as `parent`.
  RAW_PTR_EXCLUSION const WrapperTypeInfo* interface_object_parent;
  // Installs, once a context has made the interface's objects, the members
  // that the context decides, such as those of a secure context, after those
  // of the template, as Blink's InstallContextDependentProperties() does.
  InstallContextDependentPropertiesFunction
      install_context_dependent_properties;
};

}  // namespace mitto::bindings

#endif  // MITTO_BINDINGS_WRAPPER_TYPE_INFO_H_
