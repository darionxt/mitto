// What the Rust xml crate's reader calls back as it reads, through the cxx
// bridge in xml_ffi.rs, as Blink's XmlCallbacks has it: the parser that
// builds a document implements it.
//
// Upstream: third_party/blink/renderer/core/xml/parser/xml_ffi_callbacks.h
// Upstream: third_party/blink/renderer/core/xml/parser/xml_ffi_callbacks.cc

#ifndef MITTO_XML_XML_FFI_CALLBACKS_H_
#define MITTO_XML_XML_FFI_CALLBACKS_H_

#include <cstdint>
#include <string>

#include "third_party/rust/cxx/v1/cxx.h"

namespace mitto::xml_ffi {

struct AttributesIterator;
struct NamespacesIterator;
enum class StandaloneInfo : ::std::int8_t;

class XmlCallbacks {
 public:
  virtual void StartDocument(rust::Str version,
                             rust::Str encoding,
                             StandaloneInfo standalone_info) = 0;
  virtual void ProcessingInstruction(rust::Str target, rust::Str data) = 0;
  // `has_prefix` and `has_ns` tell a null prefix or namespace from an empty
  // one, which a rust::Str cannot.
  virtual void StartElementNs(rust::Str local_name,
                              bool has_prefix,
                              rust::Str prefix,
                              bool has_ns,
                              rust::Str ns,
                              AttributesIterator& attributes,
                              NamespacesIterator& namespaces) = 0;
  virtual void EndElementNs(rust::Str local_name,
                            rust::Str prefix,
                            rust::Str ns) = 0;
  virtual void Characters(rust::Str characters) = 0;
  virtual void CData(rust::Str data) = 0;
  virtual void Comment(rust::Str comment) = 0;
  virtual void DocType(rust::Str name,
                       rust::Str public_id,
                       rust::Str system_id) = 0;
  virtual void EndDocument() = 0;

 protected:
  ~XmlCallbacks() = default;
};

// One attribute that attributes_next() gives: its qualified name, its
// namespace, empty for none, and its value.
struct AttributeView {
  std::u16string q_name;
  std::u16string attr_ns;
  std::u16string value;

  void Populate(rust::Str q_name_a, rust::Str attr_ns_a, rust::Str value_a);
};

// What a rust::Str holds, which is UTF-8, as UTF-16.
std::u16string ToUtf16(rust::Str str);

}  // namespace mitto::xml_ffi

#endif  // MITTO_XML_XML_FFI_CALLBACKS_H_
