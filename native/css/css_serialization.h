// How Blink writes CSS identifiers and numbers back:
// https://drafts.csswg.org/cssom/#serialize-an-identifier
//
// Upstream: third_party/blink/renderer/core/css/css_markup.cc
// Upstream: third_party/blink/renderer/core/css/css_numeric_literal_value.cc

#ifndef MITTO_CSS_CSS_SERIALIZATION_H_
#define MITTO_CSS_CSS_SERIALIZATION_H_

#include <string>
#include <string_view>

#include "mitto/css/css_token.h"

namespace mitto {

// Appends `identifier`, escaped as CSSOM's serialize an identifier says.
void SerializeIdentifier(std::u16string_view identifier,
                         std::u16string& append_to);

// A numeric value as Blink's CSSNumericLiteralValue writes it: an integer as
// the nearest int, and any other value with at most six significant digits
// and its unit, such as "0.833333dppx" or "4.29497e+09".
std::u16string NumericLiteralCssText(double value, CssUnit unit);

}  // namespace mitto

#endif  // MITTO_CSS_CSS_SERIALIZATION_H_
