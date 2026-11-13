#include "mitto/dom/keyboard_layout_map.h"

#include <algorithm>
#include <map>
#include <string>
#include <string_view>

#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/v8_keyboard_layout_map.h"
#include "mitto/dom/string_hash_table_order.h"

namespace mitto {

namespace {

struct KeyMapping {
  std::string_view code;
  std::string_view key;
};

// What the US layout writes for each of kWritingSystemKeyDomCodes, as
// DomKeyboardLayoutMapWin reads it with ToUnicodeEx() and Chrome 157 gave it
// on Windows: IntlYen and IntlRo write nothing there, so they are left out.
constexpr KeyMapping kUsLayout[] = {
    {"Backquote", "`"},    {"Digit1", "1"},         {"Digit2", "2"},
    {"Digit3", "3"},       {"Digit4", "4"},         {"Digit5", "5"},
    {"Digit6", "6"},       {"Digit7", "7"},         {"Digit8", "8"},
    {"Digit9", "9"},       {"Digit0", "0"},         {"Minus", "-"},
    {"Equal", "="},        {"KeyQ", "q"},           {"KeyW", "w"},
    {"KeyE", "e"},         {"KeyR", "r"},           {"KeyT", "t"},
    {"KeyY", "y"},         {"KeyU", "u"},           {"KeyI", "i"},
    {"KeyO", "o"},         {"KeyP", "p"},           {"BracketLeft", "["},
    {"BracketRight", "]"}, {"Backslash", "\\"},     {"KeyA", "a"},
    {"KeyS", "s"},         {"KeyD", "d"},           {"KeyF", "f"},
    {"KeyG", "g"},         {"KeyH", "h"},           {"KeyJ", "j"},
    {"KeyK", "k"},         {"KeyL", "l"},           {"Semicolon", ";"},
    {"Quote", "'"},        {"IntlBackslash", "\\"}, {"KeyZ", "z"},
    {"KeyX", "x"},         {"KeyC", "c"},           {"KeyV", "v"},
    {"KeyB", "b"},         {"KeyN", "n"},           {"KeyM", "m"},
    {"Comma", ","},        {"Period", "."},         {"Slash", "/"},
};

// The entries in the order that a KeyboardLayoutMap iterates them: the
// browser's base::flat_map sends them sorted by code, mojo inserts each into
// a new HashMap, which grows as it fills, and the map copies its buckets.
const std::vector<std::pair<std::u16string, std::u16string>>& OrderedEntries() {
  static const base::NoDestructor<
      std::vector<std::pair<std::u16string, std::u16string>>>
      entries([] {
        std::map<std::string_view, std::string_view> sorted;
        for (const KeyMapping& mapping : kUsLayout) {
          sorted.emplace(mapping.code, mapping.key);
        }
        StringHashTableOrder table;
        for (const auto& [code, key] : sorted) {
          table.Insert(code);
        }
        std::vector<std::pair<std::u16string, std::u16string>> ordered;
        for (const std::string& code : table.Keys()) {
          ordered.emplace_back(base::UTF8ToUTF16(code),
                               base::UTF8ToUTF16(sorted.at(code)));
        }
        return ordered;
      }());
  return *entries;
}

}  // namespace

// static
KeyboardLayoutMap* KeyboardLayoutMap::Create() {
  return bindings::MakeGarbageCollected<KeyboardLayoutMap>();
}

KeyboardLayoutMap::KeyboardLayoutMap() = default;
KeyboardLayoutMap::~KeyboardLayoutMap() = default;

const bindings::WrapperTypeInfo* KeyboardLayoutMap::GetWrapperTypeInfo() const {
  return &bindings::V8KeyboardLayoutMap::kWrapperTypeInfo;
}

std::vector<std::pair<std::u16string, std::u16string>>
KeyboardLayoutMap::MaplikeEntries() const {
  return OrderedEntries();
}

}  // namespace mitto
