// What the bindings know of each interface, namespace and callback interface
// that a page can reach: its name, its parent, the tags of its objects and how
// to make its template. The bindings generator writes one for each, as Blink's
// writes its WrapperTypeInfo: with generated install functions for an
// interface that has a C++ class, and with a table of its members, which the
// functions of interface_table.h install, for every other.
//
// Upstream: third_party/blink/renderer/platform/bindings/wrapper_type_info.h

#ifndef MITTO_BINDINGS_WRAPPER_TYPE_INFO_H_
#define MITTO_BINDINGS_WRAPPER_TYPE_INFO_H_

#include <stdint.h>

#include "base/memory/raw_ptr_exclusion.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-sandbox.h"

namespace mitto::bindings {

struct InterfaceTable;
class ScriptWrappable;

struct WrapperTypeInfo : v8::Object::WrapperTypeInfo {
  // What kind of definition it is, which decides its template, as Blink's
  // WrapperTypeInfo::IdlDefinitionKind does.
  enum class Kind : uint8_t {
    // A function template, whose function is the interface object.
    kInterface,
    // A callback interface with constants: a function template without a
    // prototype, whose function only holds the constants.
    kCallbackInterface,
    // An object template, whose object is the namespace object.
    kNamespace,
    // The iterator of an iterable, maplike, setlike or async iterable
    // declaration: a function template whose prototype inherits from one of
    // V8's iterator prototypes, and that no global object exposes.
    kIterator,
  };

  // Installs the members on the template, which is a function template for an
  // interface and a callback interface and an object template for a
  // namespace.
  using InstallInterfaceTemplateFunction = void (*)(v8::Isolate*,
                                                    v8::Local<v8::Template>,
                                                    const WrapperTypeInfo&);
  // Installs what a context decides on that context's interface object,
  // prototype and, for a global interface, the global object, which is empty
  // for any other. A namespace has its object for `interface_object` and an
  // empty prototype.
  using InstallContextDependentPropertiesFunction =
      void (*)(v8::Local<v8::Context> context,
               v8::Local<v8::Object> interface_object,
               v8::Local<v8::Object> prototype_object,
               v8::Local<v8::Object> instance_object,
               const WrapperTypeInfo& info);
  // Makes an object of the class with nothing given, whose wrapper is of
  // `info`: the class's own or that of an interface without a class that
  // inherits from it.
  using CreateForKnownGapFunction =
      ScriptWrappable* (*)(const WrapperTypeInfo& info);

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
  Kind kind;
  // A number of its own among all of them, by which each context keeps the
  // objects it made for it.
  uint16_t index;
  // The members of an interface without a C++ class, which the install
  // functions above read. Null for one with a class, whose install functions
  // the generator writes.
  // RAW_PTR_EXCLUSION: #global-scope, as `parent`.
  RAW_PTR_EXCLUSION const InterfaceTable* table;
  // For an interface with a C++ class that a Known Gap may make an object of
  // (`made_by_known_gaps` in interfaces.gni), the function that makes it:
  // known_gap.h's NewObjectOfInterface() calls it for the class and for an
  // interface without a class that inherits from it. Null for any other.
  CreateForKnownGapFunction create_for_known_gap;
};

}  // namespace mitto::bindings

#endif  // MITTO_BINDINGS_WRAPPER_TYPE_INFO_H_
