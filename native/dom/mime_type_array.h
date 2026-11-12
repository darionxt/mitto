// The HTML Standard's MimeTypeArray, navigator.mimeTypes: the two mime types
// of the first of navigator.plugins, made once, as Blink's DOMMimeTypeArray
// takes them from DOMPluginArray::GetFixedMimeTypeArray().
//
// Upstream: third_party/blink/renderer/modules/plugins/dom_mime_type_array.cc
// Upstream: third_party/blink/renderer/modules/plugins/dom_plugin_array.cc

#ifndef MITTO_DOM_MIME_TYPE_ARRAY_H_
#define MITTO_DOM_MIME_TYPE_ARRAY_H_

#include <stdint.h>

#include <array>
#include <string>
#include <vector>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class MimeType;
class PluginArray;

class MimeTypeArray final : public bindings::ScriptWrappable {
 public:
  // The mime types of the first of `plugins`, the window's navigator.plugins.
  static MimeTypeArray* Create(PluginArray* plugins);

  explicit MimeTypeArray(PluginArray* plugins);
  ~MimeTypeArray() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface. The same mime type each time.
  uint32_t Length() const;
  MimeType* Item(uint32_t index);
  MimeType* NamedItem(const std::u16string& name);

  // The supported property names: the mime types' types, in order.
  std::vector<std::u16string> NamedPropertyEnumerator() const;
  bool NamedPropertyQuery(const std::u16string& name) const;

 private:
  // In the object, for cppgc to trace (see EventTarget's listeners).
  std::array<cppgc::Member<MimeType>, 2> mime_types_;
};

}  // namespace mitto

#endif  // MITTO_DOM_MIME_TYPE_ARRAY_H_
