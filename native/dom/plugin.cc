#include "mitto/dom/plugin.h"

#include <array>
#include <utility>

#include "mitto/bindings/v8_plugin.h"
#include "mitto/dom/mime_type.h"
#include "mitto/dom/plugin_array.h"

namespace mitto {
namespace {

// What MakeFakePlugin() gives each plugin of the PDF viewer.
constexpr char16_t kDescription[] = u"Portable Document Format";
constexpr char16_t kFilename[] = u"internal-pdf-viewer";
constexpr std::array<const char16_t*, 2> kMimeTypes = {u"application/pdf",
                                                       u"text/pdf"};
constexpr char16_t kSuffixes[] = u"pdf";

}  // namespace

// static
Plugin* Plugin::Create(PluginArray* plugins, std::u16string name) {
  return bindings::MakeGarbageCollected<Plugin>(plugins, std::move(name));
}

Plugin::Plugin(PluginArray* plugins, std::u16string name)
    : plugins_(plugins), name_(std::move(name)) {}

Plugin::~Plugin() = default;

const bindings::WrapperTypeInfo* Plugin::GetWrapperTypeInfo() const {
  return &bindings::V8Plugin::kWrapperTypeInfo;
}

void Plugin::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(plugins_);
}

std::u16string Plugin::Name() const {
  return name_;
}

std::u16string Plugin::Filename() const {
  return kFilename;
}

std::u16string Plugin::Description() const {
  return kDescription;
}

uint32_t Plugin::Length() const {
  return kMimeTypes.size();
}

MimeType* Plugin::Item(uint32_t index) {
  if (index >= kMimeTypes.size()) {
    return nullptr;
  }
  return MimeType::Create(plugins_.Get(), kMimeTypes[index], kSuffixes,
                          kDescription, name_);
}

MimeType* Plugin::NamedItem(const std::u16string& name) {
  for (const char16_t* type : kMimeTypes) {
    if (name == type) {
      return MimeType::Create(plugins_.Get(), type, kSuffixes, kDescription,
                              name_);
    }
  }
  return nullptr;
}

std::vector<std::u16string> Plugin::NamedPropertyEnumerator() const {
  return std::vector<std::u16string>(kMimeTypes.begin(), kMimeTypes.end());
}

bool Plugin::NamedPropertyQuery(const std::u16string& name) const {
  for (const char16_t* type : kMimeTypes) {
    if (name == type) {
      return true;
    }
  }
  return false;
}

}  // namespace mitto
