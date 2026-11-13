#include "mitto/encoding/text_codec.h"

#include "base/notreached.h"
#include "base/strings/stringprintf.h"
#include "third_party/icu/source/common/unicode/utf16.h"

namespace mitto {

// static
std::string TextCodec::GetUnencodableReplacement(int32_t code_point,
                                                 UnencodableHandling handling) {
  // Blink prints the code point with "%u" and "%x", which read it unsigned.
  const auto value = static_cast<uint32_t>(code_point);
  switch (handling) {
    case UnencodableHandling::kXmlCharRef:
      return base::StringPrintf("&#%u;", value);
    case UnencodableHandling::kUrlEncodedCharRef:
      return base::StringPrintf("%%26%%23%u%%3B", value);
    case UnencodableHandling::kCssEscape:
      return base::StringPrintf("\\%x ", value);
    case UnencodableHandling::kNone:
      break;
  }
  NOTREACHED();
}

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
