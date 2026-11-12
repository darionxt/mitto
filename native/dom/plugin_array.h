// The HTML Standard's PluginArray, navigator.plugins: the five plugins of the
// PDF viewer that Chrome lists for a page, as Blink's DOMPluginArray does
// once the viewer handles PDFs, which it does in a new profile.
//
// Upstream: third_party/blink/renderer/modules/plugins/dom_plugin_array.cc

#ifndef MITTO_DOM_PLUGIN_ARRAY_H_
#define MITTO_DOM_PLUGIN_ARRAY_H_

#include <stdint.h>

#include <array>
#include <string>
#include <vector>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class Plugin;

class PluginArray final : public bindings::ScriptWrappable {
 public:
  // How many plugins Chrome lists.
  static constexpr size_t kLength = 5;

  static PluginArray* Create();

  PluginArray();
  ~PluginArray() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface. The same plugin each time, as Blink keeps
  // them.
  uint32_t Length() const;
  Plugin* Item(uint32_t index);
  Plugin* NamedItem(const std::u16string& name);

  // The supported property names: the plugins' names, in order.
  std::vector<std::u16string> NamedPropertyEnumerator() const;
  bool NamedPropertyQuery(const std::u16string& name) const;

 private:
  // Made the first time a page asks for one, since each plugin keeps this
  // object, which hands out its mime types' enabledPlugin.
  void MakePlugins();

  // In the object, not in a std::vector's buffer, for cppgc to trace (see
  // EventTarget's listeners).
  std::array<cppgc::Member<Plugin>, kLength> plugins_;
};

}  // namespace mitto

#endif  // MITTO_DOM_PLUGIN_ARRAY_H_
