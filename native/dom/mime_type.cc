#include "mitto/dom/mime_type.h"

#include <utility>

#include "mitto/bindings/v8_mime_type.h"
#include "mitto/dom/plugin_array.h"

namespace mitto {

// static
MimeType* MimeType::Create(PluginArray* plugins,
                           std::u16string type,
                           std::u16string suffixes,
                           std::u16string description,
                           std::u16string plugin_name) {
  return bindings::MakeGarbageCollected<MimeType>(
      plugins, std::move(type), std::move(suffixes), std::move(description),
      std::move(plugin_name));
}

MimeType::MimeType(PluginArray* plugins,
                   std::u16string type,
                   std::u16string suffixes,
                   std::u16string description,
                   std::u16string plugin_name)
    : plugins_(plugins),
      type_(std::move(type)),
      suffixes_(std::move(suffixes)),
      description_(std::move(description)),
      plugin_name_(std::move(plugin_name)) {}

MimeType::~MimeType() = default;

const bindings::WrapperTypeInfo* MimeType::GetWrapperTypeInfo() const {
  return &bindings::V8MimeType::kWrapperTypeInfo;
}

void MimeType::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(plugins_);
}

std::u16string MimeType::Type() const {
  return type_;
}

std::u16string MimeType::Suffixes() const {
  return suffixes_;
}

std::u16string MimeType::Description() const {
  return description_;
}

Plugin* MimeType::EnabledPlugin() const {
  return plugins_->NamedItem(plugin_name_);
}

}  // namespace mitto
