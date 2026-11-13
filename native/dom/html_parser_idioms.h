// The HTML Standard's microsyntaxes that the DOM's classes read their content
// attributes with.
//
// Upstream: third_party/blink/renderer/core/html/parser/html_parser_idioms.h
// Upstream: third_party/blink/renderer/core/html/parser/html_parser_idioms.cc

#ifndef MITTO_DOM_HTML_PARSER_IDIOMS_H_
#define MITTO_DOM_HTML_PARSER_IDIOMS_H_

#include <stdint.h>

#include <optional>
#include <string_view>

namespace mitto {

// ASCII whitespace, Blink's IsHTMLSpace().
bool IsHTMLSpace(char16_t c);

// `value` without its leading and trailing HTML spaces, Blink's
// StripLeadingAndTrailingHtmlSpaces().
std::u16string_view StripHTMLSpaces(std::u16string_view value);

// Blink's ParseHTMLInteger(): the rules for parsing integers, with what
// follows the digits ignored, and nothing for no digits or a value beyond an
// int.
std::optional<int32_t> ParseHTMLInteger(std::u16string_view value);

// Blink's ParseHTMLNonNegativeInteger(): the rules for parsing non-negative
// integers up to 4294967295, which take "-0" and ignore what follows the
// digits.
std::optional<uint32_t> ParseHTMLNonNegativeInteger(std::u16string_view value);

// Blink's ParseHTMLClampedNonNegativeInteger(): the same, clamped to `min`
// and `max`, with `max` for a value beyond 4294967295.
std::optional<uint32_t> ParseHTMLClampedNonNegativeInteger(
    std::u16string_view value,
    uint32_t min,
    uint32_t max);

}  // namespace mitto

#endif  // MITTO_DOM_HTML_PARSER_IDIOMS_H_
