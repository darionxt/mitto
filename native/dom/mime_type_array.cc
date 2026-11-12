#include "mitto/dom/mime_type_array.h"

#include "base/check_op.h"
#include "mitto/bindings/v8_mime_type_array.h"
#include "mitto/dom/mime_type.h"
#include "mitto/dom/plugin.h"
#include "mitto/dom/plugin_array.h"

namespace mitto {

// static
MimeTypeArray* MimeTypeArray::Create(PluginArray* plugins) {
  return bindings::MakeGarbageCollected<MimeTypeArray>(plugins);
}

MimeTypeArray::MimeTypeArray(PluginArray* plugins) {
  // GetFixedMimeTypeArray(): new mime types of the first plugin.
  Plugin* const first = plugins->Item(0);
  DCHECK_EQ(first->Length(), mime_types_.size());
  for (uint32_t i = 0; i < mime_types_.size(); ++i) {
    mime_types_[i] = first->Item(i);
  }
}

MimeTypeArray::~MimeTypeArray() = default;

const bindings::WrapperTypeInfo* MimeTypeArray::GetWrapperTypeInfo() const {
  return &bindings::V8MimeTypeArray::kWrapperTypeInfo;
}

void MimeTypeArray::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  for (const cppgc::Member<MimeType>& mime_type : mime_types_) {
    visitor->Trace(mime_type);
  }
}

uint32_t MimeTypeArray::Length() const {
  return mime_types_.size();
}

MimeType* MimeTypeArray::Item(uint32_t index) {
  if (index >= mime_types_.size()) {
    return nullptr;
  }
  return mime_types_[index].Get();
}

MimeType* MimeTypeArray::NamedItem(const std::u16string& name) {
  for (const cppgc::Member<MimeType>& mime_type : mime_types_) {
    if (mime_type->Type() == name) {
      return mime_type.Get();
    }
  }
  return nullptr;
}

std::vector<std::u16string> MimeTypeArray::NamedPropertyEnumerator() const {
  std::vector<std::u16string> names;
  for (const cppgc::Member<MimeType>& mime_type : mime_types_) {
    names.push_back(mime_type->Type());
  }
  return names;
}

bool MimeTypeArray::NamedPropertyQuery(const std::u16string& name) const {
  for (const cppgc::Member<MimeType>& mime_type : mime_types_) {
    if (mime_type->Type() == name) {
      return true;
    }
  }
  return false;
}

}  // namespace mitto
