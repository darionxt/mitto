// The indexed and named properties of an interface whose C++ class has an
// indexed or a named property getter: what makes its objects Web IDL's legacy
// platform objects, such as navigator.plugins, whose own properties include
// "0" and "PDF Viewer".
//
// V8 asks interceptors on the interface's instance template for these
// properties. The bindings generator writes, for such an interface, the
// functions of an IndexedAndNamedProperties that read its class, and the
// interceptors here do the rest of Web IDL's internal methods of a legacy
// platform object as bind_gen's do: [[GetOwnProperty]], [[Set]],
// [[DefineOwnProperty]], [[Delete]] and [[OwnPropertyKeys]], with Chrome's
// errors.
//
// Upstream: third_party/blink/renderer/bindings/scripts/bind_gen/interface.py
// Upstream: third_party/blink/renderer/bindings/core/v8/generated_code_helper.cc
// Upstream: third_party/blink/renderer/platform/bindings/v8_set_return_value.cc

#ifndef MITTO_BINDINGS_INDEXED_AND_NAMED_PROPERTIES_H_
#define MITTO_BINDINGS_INDEXED_AND_NAMED_PROPERTIES_H_

#include <stdint.h>

#include <string>
#include <vector>

#include "v8/include/v8-forward.h"

namespace mitto::bindings {

// IndexedAndNamedProperties::flags.
// [LegacyUnenumerableNamedProperties]: the named properties are not
// enumerable.
inline constexpr uint8_t kNamedPropertiesUnenumerable = 1 << 0;
// The getters have side effects, [Affects=Everything], so V8's inspector does
// not call them to preview an object.
inline constexpr uint8_t kIndexedGetterHasSideEffects = 1 << 1;
inline constexpr uint8_t kNamedGetterHasSideEffects = 1 << 2;

// What the class of an interface gives: each function reads the object that
// `holder` wraps, which is one of the interface's.
struct IndexedAndNamedProperties {
  // The class's length(): the supported property indices are those below it.
  // Null without an indexed property getter.
  uint32_t (*length)(v8::Isolate* isolate, v8::Local<v8::Object> holder);
  // What the indexed property getter gives for an index below the length.
  v8::Local<v8::Value> (*indexed_getter)(v8::Isolate* isolate,
                                         v8::Local<v8::Object> holder,
                                         uint32_t index);
  // What the named property getter gives for `name`, or an empty handle when
  // `name` is not a supported property name. Null without a named property
  // getter.
  v8::Local<v8::Value> (*named_getter)(v8::Isolate* isolate,
                                       v8::Local<v8::Object> holder,
                                       const std::u16string& name);
  // The supported property names, in order, as Blink's
  // NamedPropertyEnumerator() gives them.
  std::vector<std::u16string> (*names)(v8::Isolate* isolate,
                                       v8::Local<v8::Object> holder);
  // Whether `name` is a supported property name, as Blink's
  // NamedPropertyQuery() says.
  bool (*has_name)(v8::Isolate* isolate,
                   v8::Local<v8::Object> holder,
                   const std::u16string& name);
  uint8_t flags;
};

// Installs the interceptors of `properties` on the interface's instance
// template, as bind_gen's make_indexed_and_named_property_callbacks_and_
// install_node() does for an interface without indexed or named property
// setters and deleters.
void InstallIndexedAndNamedProperties(
    v8::Isolate* isolate,
    v8::Local<v8::ObjectTemplate> instance_template,
    const IndexedAndNamedProperties& properties);

}  // namespace mitto::bindings

#endif  // MITTO_BINDINGS_INDEXED_AND_NAMED_PROPERTIES_H_
