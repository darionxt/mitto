#include "mitto/dom/plugin_array.h"

#include <array>

#include "mitto/bindings/v8_plugin_array.h"
#include "mitto/dom/plugin.h"

namespace mitto {
namespace {

// The spec'd, hard-coded list that Blink gives a page whose browser views
// PDFs (crbug.com/1164635).
constexpr std::array<const char16_t*, PluginArray::kLength> kPluginNames = {
    u"PDF Viewer", u"Chrome PDF Viewer", u"Chromium PDF Viewer",
    u"Microsoft Edge PDF Viewer", u"WebKit built-in PDF"};

}  // namespace

// static
PluginArray* PluginArray::Create() {
  return bindings::MakeGarbageCollected<PluginArray>();
}

PluginArray::PluginArray() = default;

PluginArray::~PluginArray() = default;

const bindings::WrapperTypeInfo* PluginArray::GetWrapperTypeInfo() const {
  return &bindings::V8PluginArray::kWrapperTypeInfo;
}

void PluginArray::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  for (const cppgc::Member<Plugin>& plugin : plugins_) {
    visitor->Trace(plugin);
  }
}

void PluginArray::MakePlugins() {
  if (plugins_[0]) {
    return;
  }
  for (size_t i = 0; i < kLength; ++i) {
    plugins_[i] = Plugin::Create(this, kPluginNames[i]);
  }
}

uint32_t PluginArray::Length() const {
  return kLength;
}

Plugin* PluginArray::Item(uint32_t index) {
  if (index >= kLength) {
    return nullptr;
  }
  MakePlugins();
  return plugins_[index].Get();
}

Plugin* PluginArray::NamedItem(const std::u16string& name) {
  MakePlugins();
  for (const cppgc::Member<Plugin>& plugin : plugins_) {
    if (plugin->Name() == name) {
      return plugin.Get();
    }
  }
  return nullptr;
}

std::vector<std::u16string> PluginArray::NamedPropertyEnumerator() const {
  return std::vector<std::u16string>(kPluginNames.begin(), kPluginNames.end());
}

bool PluginArray::NamedPropertyQuery(const std::u16string& name) const {
  for (const char16_t* plugin_name : kPluginNames) {
    if (name == plugin_name) {
      return true;
    }
  }
  return false;
}

}  // namespace mitto
