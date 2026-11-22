// The @import rules of a style sheet, as Blink's CSSParserImpl takes them
// from the top level: each before any rule that parses, but for @charset and
// @layer statements before it, so that a rule that does not parse ends no
// import section. M9's CSS parser takes this over.
//
// Upstream: third_party/blink/renderer/core/css/parser/css_parser_impl.cc
// Upstream: third_party/blink/renderer/core/css/style_rule_import.cc
// Upstream: third_party/blink/renderer/core/css/parser/css_supports_parser.cc
// Upstream: third_party/blink/renderer/core/css/parser/container_query_parser.cc
// Upstream: third_party/blink/renderer/core/css/css_syntax_string_parser.cc
// Upstream: third_party/blink/renderer/core/css/properties/css_parsing_utils.cc

#ifndef MITTO_CSS_STYLE_SHEET_IMPORTS_H_
#define MITTO_CSS_STYLE_SHEET_IMPORTS_H_

#include <string>
#include <string_view>
#include <vector>

#include "mitto/css/css_selector_parser.h"

namespace mitto {

struct StyleSheetImport {
  // The URL as written, before it resolves against the sheet's. Its layer,
  // supports() and media decide nothing of its fetch here: a false
  // supports(), which loads nothing in Chrome, is a Known Gap.
  std::u16string url;
};

// The imports of `text`, in order, with the selectors of style rules parsed
// in `context`, which decides whether a rule counts.
std::vector<StyleSheetImport> FindStyleSheetImports(
    std::u16string_view text,
    const CssSelectorParserContext& context);

}  // namespace mitto

#endif  // MITTO_CSS_STYLE_SHEET_IMPORTS_H_
