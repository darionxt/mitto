#include "mitto/xml/xml_ffi_callbacks.h"

#include <string_view>

#include "base/strings/utf_string_conversions.h"

namespace mitto::xml_ffi {

std::u16string ToUtf16(rust::Str str) {
  return base::UTF8ToUTF16(std::string_view(str.data(), str.size()));
}

void AttributeView::Populate(rust::Str q_name_a,
                             rust::Str attr_ns_a,
                             rust::Str value_a) {
  q_name = ToUtf16(q_name_a);
  attr_ns = ToUtf16(attr_ns_a);
  value = ToUtf16(value_a);
}

}  // namespace mitto::xml_ffi
