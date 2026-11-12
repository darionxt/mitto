// The HTML Standard's Plugin: one of navigator.plugins, a name for Chrome's
// PDF viewer, with the viewer's two mime types, as Blink's DOMPlugin made by
// DOMPluginArray's MakeFakePlugin().
//
// Upstream: third_party/blink/renderer/modules/plugins/dom_plugin.cc
// Upstream: third_party/blink/renderer/modules/plugins/dom_plugin_array.cc

#ifndef MITTO_DOM_PLUGIN_H_
#define MITTO_DOM_PLUGIN_H_

#include <stdint.h>

#include <string>
#include <vector>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class MimeType;
class PluginArray;

class Plugin final : public bindings::ScriptWrappable {
 public:
  // The plugin named `name` of `plugins`, the window's navigator.plugins.
  static Plugin* Create(PluginArray* plugins, std::u16string name);

  Plugin(PluginArray* plugins, std::u16string name);
  ~Plugin() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface. Item() and NamedItem() make a new mime type
  // each time, as Blink does.
  std::u16string Name() const;
  std::u16string Filename() const;
  std::u16string Description() const;
  uint32_t Length() const;
  MimeType* Item(uint32_t index);
  MimeType* NamedItem(const std::u16string& name);

  // The supported property names: the mime types' types, in order.
  std::vector<std::u16string> NamedPropertyEnumerator() const;
  bool NamedPropertyQuery(const std::u16string& name) const;

 private:
  const cppgc::Member<PluginArray> plugins_;
  const std::u16string name_;
};

}  // namespace mitto

#endif  // MITTO_DOM_PLUGIN_H_
