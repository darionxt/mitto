// The HTML Standard's MimeType: one of the PDF viewer's two mime types, as
// Blink's DOMMimeType gives them.
//
// Upstream: third_party/blink/renderer/modules/plugins/dom_mime_type.cc

#ifndef MITTO_DOM_MIME_TYPE_H_
#define MITTO_DOM_MIME_TYPE_H_

#include <string>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class Plugin;
class PluginArray;

class MimeType final : public bindings::ScriptWrappable {
 public:
  // A mime type of the plugin named `plugin_name` of `plugins`, the window's
  // navigator.plugins.
  static MimeType* Create(PluginArray* plugins,
                          std::u16string type,
                          std::u16string suffixes,
                          std::u16string description,
                          std::u16string plugin_name);

  MimeType(PluginArray* plugins,
           std::u16string type,
           std::u16string suffixes,
           std::u16string description,
           std::u16string plugin_name);
  ~MimeType() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  std::u16string Type() const;
  std::u16string Suffixes() const;
  std::u16string Description() const;
  // The plugin of navigator.plugins that has its plugin's name, as Blink
  // looks it up: a page lets plugins run unless its settings say otherwise,
  // which a Session's do not.
  Plugin* EnabledPlugin() const;

 private:
  const cppgc::Member<PluginArray> plugins_;
  const std::u16string type_;
  const std::u16string suffixes_;
  const std::u16string description_;
  const std::u16string plugin_name_;
};

}  // namespace mitto

#endif  // MITTO_DOM_MIME_TYPE_H_
