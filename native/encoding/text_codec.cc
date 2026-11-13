#include "mitto/encoding/text_codec.h"

#include "third_party/icu/source/common/unicode/utf16.h"

namespace mitto {

// Upstream: third_party/blink/renderer/platform/wtf/text/string_builder.h
void AppendCodePoint(std::u16string& result, int32_t code_point) {
  if (U_IS_BMP(code_point)) {
    result.push_back(static_cast<char16_t>(code_point));
    return;
  }
  result.push_back(U16_LEAD(code_point));
  result.push_back(U16_TRAIL(code_point));
}

}  // namespace mitto
