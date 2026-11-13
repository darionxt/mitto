// The selector parser, as Blink's CSSSelectorParser parses a selector list
// that querySelector(), matches() and closest() take:
// https://drafts.csswg.org/selectors/#parse-selector
//
// It reads the tokens of CSS Syntax's tokenizer through a CssTokenStream, as
// Blink's reads a CSSParserTokenStream, and follows Blink in what it takes:
// the pseudo-classes and pseudo-elements of Blink's tables that the
// runtime-enabled features it is given allow, forgiving lists in :is() and
// :where(), relative selectors in :has(), and Blink's limits on what may
// follow a pseudo-element. There is no style sheet, so no namespace prefix
// but "*" and the empty one resolves, and nothing is nested: & matches as
// :scope does.
//
// Upstream: third_party/blink/renderer/core/css/parser/css_selector_parser.cc
// Upstream: third_party/blink/renderer/core/css/css_selector.cc
// Upstream: third_party/blink/renderer/core/css/properties/css_parsing_utils.cc

#ifndef MITTO_CSS_CSS_SELECTOR_PARSER_H_
#define MITTO_CSS_CSS_SELECTOR_PARSER_H_

#include <memory>
#include <string_view>

#include "mitto/css/css_selector.h"

namespace mitto {

// The runtime-enabled features that decide which pseudo-classes and
// pseudo-elements parse and what follows them, as Chrome's stable set on
// Windows has them unless a Page's set says else.
struct CssSelectorFeatures {
  bool media_element_pseudos = false;  // CSSMediaElementPseudos
  // GeolocationElement, UserMediaElement or InstallElement, for :granted.
  bool permission_elements = true;
  bool pseudo_scroll_markers = true;              // CSSPseudoScrollMarkers
  bool scroll_marker_target_before_after = true;  // CSSScrollMarker...
  bool pseudo_scroll_buttons = true;              // CSSPseudoScrollButtons
  bool pseudo_column = true;                      // CSSPseudoColumn
  bool search_text_highlight = true;              // SearchTextHighlightPseudo
  // DeclarativeWebMCP and WebMCP, for :tool-form-active and
  // :tool-submit-active.
  bool declarative_web_mcp = false;
  bool unbounded_element = true;         // UnboundedElement
  bool pseudo_has_slotted = false;       // CSSPseudoHasSlotted
  bool overscroll_areas = false;         // OverscrollAreas
  bool customizable_combobox = false;    // CustomizableCombobox
  bool filterable_select = false;        // FilterableSelect
  bool image_animation = false;          // CSSImageAnimation
  bool menu_elements = false;            // MenuElements
  bool navigation_source = true;         // NavigationSourcePseudoClass
  bool declarative_skeletons = false;    // DeclarativeSkeletons
  bool marker_nested = true;             // CSSMarkerNestedPseudoElement
  bool lang_extended_ranges = false;     // CSSLangExtendedRanges
  bool case_sensitive_selector = false;  // CSSCaseSensitiveSelector
  bool highlight_universal = false;      // CSSCustomHighlightUniversalSelector
  bool pseudo_elements_hoverable = false;  // PseudoElementsHoverable
  bool route_matching = false;             // RouteMatching
  bool revert_rule = true;                 // CSSRevertRule
};

// What the document that a selector is for decides, as Blink's
// CSSParserContext has it for a document.
struct CssSelectorParserContext {
  // Type selectors and attribute names are put in lower case in an HTML
  // document.
  bool html_document = true;
  // IDs and classes are put in lower case in a document in quirks mode.
  bool quirks_mode = false;
  CssSelectorFeatures features;
};

// Parses `text` as a selector list, or returns null if it is not one.
std::unique_ptr<CssSelectorList> ParseCssSelectorList(
    std::u16string_view text,
    const CssSelectorParserContext& context);

}  // namespace mitto

#endif  // MITTO_CSS_CSS_SELECTOR_PARSER_H_
