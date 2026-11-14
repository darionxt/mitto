#include "mitto/dom/selector_query.h"

#include <utility>
#include <vector>

#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/css/css_selector.h"
#include "mitto/css/css_selector_parser.h"
#include "mitto/dom/document.h"
#include "mitto/dom/element.h"
#include "mitto/dom/selector_checker.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

namespace {

// The runtime-enabled features of the Page that decide what parses.
CssSelectorFeatures FeaturesOfPage() {
  using bindings::RuntimeFeature;
  CssSelectorFeatures result;
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (!isolate) {
    return result;
  }
  const bindings::RuntimeFeatureSet& features =
      bindings::PerIsolateData::From(isolate)->features();
  result.media_element_pseudos =
      features.IsEnabled(RuntimeFeature::kCSSMediaElementPseudos);
  result.permission_elements =
      features.IsEnabled(RuntimeFeature::kGeolocationElement) ||
      features.IsEnabled(RuntimeFeature::kUserMediaElement) ||
      features.IsEnabled(RuntimeFeature::kInstallElement);
  result.pseudo_scroll_markers =
      features.IsEnabled(RuntimeFeature::kCSSPseudoScrollMarkers);
  result.scroll_marker_target_before_after =
      features.IsEnabled(RuntimeFeature::kCSSScrollMarkerTargetBeforeAfter);
  result.pseudo_scroll_buttons =
      features.IsEnabled(RuntimeFeature::kCSSPseudoScrollButtons);
  result.pseudo_column = features.IsEnabled(RuntimeFeature::kCSSPseudoColumn);
  result.search_text_highlight =
      features.IsEnabled(RuntimeFeature::kSearchTextHighlightPseudo);
  result.declarative_web_mcp =
      features.IsEnabled(RuntimeFeature::kDeclarativeWebMCP) &&
      features.IsEnabled(RuntimeFeature::kWebMCP);
  result.unbounded_element =
      features.IsEnabled(RuntimeFeature::kUnboundedElement);
  result.pseudo_has_slotted =
      features.IsEnabled(RuntimeFeature::kCSSPseudoHasSlotted);
  result.overscroll_areas =
      features.IsEnabled(RuntimeFeature::kOverscrollAreas);
  result.customizable_combobox =
      features.IsEnabled(RuntimeFeature::kCustomizableCombobox);
  result.filterable_select =
      features.IsEnabled(RuntimeFeature::kFilterableSelect);
  result.image_animation =
      features.IsEnabled(RuntimeFeature::kCSSImageAnimation);
  result.menu_elements = features.IsEnabled(RuntimeFeature::kMenuElements);
  result.navigation_source =
      features.IsEnabled(RuntimeFeature::kNavigationSourcePseudoClass);
  result.declarative_skeletons =
      features.IsEnabled(RuntimeFeature::kDeclarativeSkeletons);
  result.marker_nested =
      features.IsEnabled(RuntimeFeature::kCSSMarkerNestedPseudoElement);
  result.lang_extended_ranges =
      features.IsEnabled(RuntimeFeature::kCSSLangExtendedRanges);
  result.case_sensitive_selector =
      features.IsEnabled(RuntimeFeature::kCSSCaseSensitiveSelector);
  result.highlight_universal =
      features.IsEnabled(RuntimeFeature::kCSSCustomHighlightUniversalSelector);
  result.pseudo_elements_hoverable =
      features.IsEnabled(RuntimeFeature::kPseudoElementsHoverable);
  result.route_matching = features.IsEnabled(RuntimeFeature::kRouteMatching);
  result.revert_rule = features.IsEnabled(RuntimeFeature::kCSSRevertRule);
  return result;
}

}  // namespace

// static
std::optional<SelectorQuery> SelectorQuery::Parse(
    const std::u16string& selectors,
    const Node& node,
    bindings::ExceptionState& exception_state) {
  if (selectors.empty()) {
    exception_state.ThrowDOMException("SyntaxError",
                                      "The provided selector is empty.");
    return std::nullopt;
  }
  const Document& document = node.GetDocument();
  CssSelectorParserContext context;
  context.html_document = document.IsHTMLDocument();
  context.quirks_mode = document.InQuirksMode();
  context.features = FeaturesOfPage();
  std::unique_ptr<CssSelectorList> list =
      ParseCssSelectorList(selectors, context);
  if (!list) {
    exception_state.ThrowDOMException(
        "SyntaxError", base::StrCat({"'", base::UTF16ToUTF8(selectors),
                                     "' is not a valid selector."}));
    return std::nullopt;
  }
  // Blink's SelectorQuery leaves out each selector with a pseudo-element.
  std::erase_if(list->complexes, [](const CssComplexSelector& complex) {
    return complex.HasPseudoElement();
  });
  return SelectorQuery(std::move(list));
}

SelectorQuery::SelectorQuery(std::unique_ptr<CssSelectorList> list)
    : list_(std::move(list)) {}

SelectorQuery::SelectorQuery(SelectorQuery&&) = default;
SelectorQuery& SelectorQuery::operator=(SelectorQuery&&) = default;
SelectorQuery::~SelectorQuery() = default;

bool SelectorQuery::Matches(const Element& element) const {
  return SelectorChecker(&element).MatchesList(*list_, element);
}

Element* SelectorQuery::Closest(const Element& element) const {
  const SelectorChecker checker(&element);
  for (Element* ancestor = const_cast<Element*>(&element); ancestor;
       ancestor = ancestor->ParentElement()) {
    if (checker.MatchesList(*list_, *ancestor)) {
      return ancestor;
    }
  }
  return nullptr;
}

Element* SelectorQuery::QueryFirst(const Node& root) const {
  if (list_->complexes.empty()) {
    return nullptr;
  }
  const SelectorChecker checker(&root);
  for (Node* node = root.FirstChild(); node; node = node->NextInTree(&root)) {
    if (node->IsElementNode() &&
        checker.MatchesList(*list_, static_cast<Element&>(*node))) {
      return static_cast<Element*>(node);
    }
  }
  return nullptr;
}

NodeVector SelectorQuery::QueryAll(const Node& root) const {
  NodeVector result;
  if (list_->complexes.empty()) {
    return result;
  }
  const SelectorChecker checker(&root);
  for (Node* node = root.FirstChild(); node; node = node->NextInTree(&root)) {
    if (node->IsElementNode() &&
        checker.MatchesList(*list_, static_cast<Element&>(*node))) {
      result.emplace_back(node);
    }
  }
  return result;
}

}  // namespace mitto
