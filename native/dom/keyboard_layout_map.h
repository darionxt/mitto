// The Keyboard Map API's KeyboardLayoutMap, which keyboard.getLayoutMap()
// gives: what each writing system key of the keyboard's layout writes with no
// modifier. Every Persona's locale is en-US, whose layout Windows sets up, so
// it is the US layout, in the order that Blink's HashMap of it iterates.
//
// Upstream: third_party/blink/renderer/modules/keyboard/keyboard_layout_map.cc
// Upstream: ui/events/keycodes/dom/dom_keyboard_layout.h
// Upstream: ui/events/keycodes/dom/dom_keyboard_layout_map_base.cc
// Upstream: ui/events/keycodes/dom/dom_keyboard_layout_map_win.cc
// Upstream: mojo/public/cpp/bindings/map_traits_wtf_hash_map.h

#ifndef MITTO_DOM_KEYBOARD_LAYOUT_MAP_H_
#define MITTO_DOM_KEYBOARD_LAYOUT_MAP_H_

#include <string>
#include <utility>
#include <vector>

#include "mitto/bindings/script_wrappable.h"

namespace mitto {

class KeyboardLayoutMap final : public bindings::ScriptWrappable {
 public:
  // A new map of the US layout, as each answer of Chrome's browser makes one.
  static KeyboardLayoutMap* Create();

  KeyboardLayoutMap();
  ~KeyboardLayoutMap() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The maplike's entries: each key's code and what it writes.
  std::vector<std::pair<std::u16string, std::u16string>> MaplikeEntries() const;
};

}  // namespace mitto

#endif  // MITTO_DOM_KEYBOARD_LAYOUT_MAP_H_
