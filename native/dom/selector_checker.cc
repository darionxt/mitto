#include "mitto/dom/selector_checker.h"

#include <algorithm>
#include <limits>
#include <string>
#include <string_view>

#include "base/notreached.h"
#include "base/strings/string_util.h"
#include "mitto/dom/character_data.h"
#include "mitto/dom/document.h"
#include "mitto/dom/element.h"
#include "mitto/dom/node.h"
#include "mitto/dom/pseudo_class_state.h"
#include "mitto/dom/qualified_name.h"

namespace mitto {

namespace {

using Match = CssSimpleSelector::Match;
using Pseudo = CssPseudoType;

bool IsHTMLSpace(char16_t c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

bool EqualsInCase(std::u16string_view a,
                  std::u16string_view b,
                  bool case_insensitive) {
  return case_insensitive ? base::EqualsCaseInsensitiveASCII(a, b) : a == b;
}

// Whether the HTML Standard has the values of an attribute of this name
// match in any case in an HTML document, in its "case-sensitivity of
// selectors", as Blink's HTMLDocument::IsCaseSensitiveAttribute() lists
// them.
bool IsLegacyCaseInsensitiveAttribute(std::u16string_view name) {
  constexpr std::u16string_view kNames[] = {
      u"accept",    u"accept-charset", u"align",     u"alink",    u"axis",
      u"bgcolor",   u"charset",        u"checked",   u"clear",    u"codetype",
      u"color",     u"compact",        u"declare",   u"defer",    u"dir",
      u"direction", u"disabled",       u"enctype",   u"face",     u"frame",
      u"hreflang",  u"http-equiv",     u"lang",      u"language", u"link",
      u"media",     u"method",         u"multiple",  u"nohref",   u"noresize",
      u"noshade",   u"nowrap",         u"readonly",  u"rel",      u"rev",
      u"rules",     u"scope",          u"scrolling", u"selected", u"shape",
      u"target",    u"text",           u"type",      u"valign",   u"valuetype",
      u"vlink"};
  return std::find(std::begin(kNames), std::end(kNames), name) !=
         std::end(kNames);
}

bool ClassListContains(std::u16string_view classes,
                       std::u16string_view name,
                       bool case_insensitive) {
  size_t start = 0;
  while (start < classes.size()) {
    while (start < classes.size() && IsHTMLSpace(classes[start])) {
      ++start;
    }
    size_t end = start;
    while (end < classes.size() && !IsHTMLSpace(classes[end])) {
      ++end;
    }
    if (end > start && EqualsInCase(classes.substr(start, end - start), name,
                                    case_insensitive)) {
      return true;
    }
    start = end;
  }
  return false;
}

// Blink's MatchesTagName() and MatchesUniversalTagName(): a name in an HTML
// document was put in lower case when it was parsed, and a foreign element
// there, such as SVG's foreignObject, matches it in any ASCII case.
bool MatchesTag(const CssSimpleSelector& selector, const Element& element) {
  if (selector.name != u"*" && selector.name != element.LocalName()) {
    if (element.IsHTMLElement() || !element.GetDocument().IsHTMLDocument() ||
        !base::EqualsCaseInsensitiveASCII(selector.name, element.LocalName())) {
      return false;
    }
  }
  switch (selector.name_space) {
    case CssSimpleSelector::Namespace::kAny:
      return true;
    case CssSimpleSelector::Namespace::kNone:
      return !element.NamespaceURI().has_value();
  }
  NOTREACHED();
}

// Blink's AttributeValueMatches().
bool AttributeValueMatches(std::u16string_view value,
                           Match match,
                           std::u16string_view selector_value,
                           bool case_insensitive) {
  switch (match) {
    case Match::kAttributeExact:
      return EqualsInCase(value, selector_value, case_insensitive);
    case Match::kAttributeSet:
      return true;
    case Match::kAttributeList:
      if (selector_value.empty() ||
          std::any_of(selector_value.begin(), selector_value.end(),
                      IsHTMLSpace)) {
        return false;
      }
      return ClassListContains(value, selector_value, case_insensitive);
    case Match::kAttributeContain:
      if (selector_value.empty() || value.size() < selector_value.size()) {
        return false;
      }
      for (size_t i = 0; i + selector_value.size() <= value.size(); ++i) {
        if (EqualsInCase(value.substr(i, selector_value.size()), selector_value,
                         case_insensitive)) {
          return true;
        }
      }
      return false;
    case Match::kAttributeBegin:
      return !selector_value.empty() && value.size() >= selector_value.size() &&
             EqualsInCase(value.substr(0, selector_value.size()),
                          selector_value, case_insensitive);
    case Match::kAttributeEnd:
      return !selector_value.empty() && value.size() >= selector_value.size() &&
             EqualsInCase(value.substr(value.size() - selector_value.size()),
                          selector_value, case_insensitive);
    case Match::kAttributeHyphen:
      if (value.size() < selector_value.size() ||
          !EqualsInCase(value.substr(0, selector_value.size()), selector_value,
                        case_insensitive)) {
        return false;
      }
      return value.size() == selector_value.size() ||
             value[selector_value.size()] == '-';
    default:
      NOTREACHED();
  }
}

// Blink's AnyAttributeMatches(). An attribute of a foreign element in an
// HTML document matches a name in any ASCII case, since the name was put in
// lower case when it was parsed.
bool AnyAttributeMatches(const CssSimpleSelector& selector,
                         const Element& element) {
  const bool in_html_document = element.GetDocument().IsHTMLDocument();
  bool case_insensitive = false;
  switch (selector.attribute_case) {
    case CssSimpleSelector::AttributeCase::kInsensitive:
      case_insensitive = true;
      break;
    case CssSimpleSelector::AttributeCase::kSensitive:
      break;
    case CssSimpleSelector::AttributeCase::kDefault:
      case_insensitive = element.IsHTMLElement() && in_html_document &&
                         IsLegacyCaseInsensitiveAttribute(selector.name);
      break;
  }
  const bool any_namespace =
      selector.name_space == CssSimpleSelector::Namespace::kAny;
  for (const Element::Attribute& attribute : element.attributes()) {
    const QualifiedName& name = attribute.name;
    const bool namespace_matches = any_namespace || !name.namespace_uri;
    if (!namespace_matches || name.local_name != selector.name) {
      if (element.IsHTMLElement() || !in_html_document || !namespace_matches ||
          !base::EqualsCaseInsensitiveASCII(name.local_name, selector.name)) {
        continue;
      }
    }
    if (AttributeValueMatches(attribute.value, selector.match, selector.value,
                              case_insensitive)) {
      return true;
    }
    if (!any_namespace) {
      return false;
    }
  }
  return false;
}

bool HasSameTag(const Element& a, const Element& b) {
  return a.LocalName() == b.LocalName() && a.NamespaceURI() == b.NamespaceURI();
}

// Blink's CSSSelector::MatchNth().
bool MatchNth(int nth_a, int nth_b, unsigned count) {
  constexpr int kMaxValue = std::numeric_limits<int>::max() / 2;
  constexpr int kMinValue = std::numeric_limits<int>::min() / 2;
  if (count > static_cast<unsigned>(kMaxValue) || nth_a > kMaxValue ||
      nth_a < kMinValue || nth_b > kMaxValue || nth_b < kMinValue) {
    return false;
  }
  const int current = static_cast<int>(count);
  if (nth_a == 0) {
    return current == nth_b;
  }
  if (nth_a > 0) {
    return current >= nth_b && (current - nth_b) % nth_a == 0;
  }
  return current <= nth_b && (nth_b - current) % (-nth_a) == 0;
}

bool IsEmpty(const Element& element) {
  for (const Node* child = element.FirstChild(); child;
       child = child->NextSibling()) {
    if (child->IsElementNode()) {
      return false;
    }
    // A text child that is not empty, even of whitespace alone, makes it not
    // empty, as in Chrome 157.
    if (child->IsTextNode() &&
        !static_cast<const CharacterData*>(child)->Data().empty()) {
      return false;
    }
  }
  return true;
}

// Blink's IsValidBCP47Value() for a language tag, which has no wildcards.
bool IsValidLanguageTag(std::u16string_view value) {
  if (value.empty()) {
    return false;
  }
  bool first = true;
  size_t position = 0;
  while (position < value.size()) {
    const size_t start = position;
    while (position < value.size() && value[position] != '-') {
      ++position;
    }
    const size_t length = position - start;
    if (length == 0 || length > 8) {
      return false;
    }
    for (size_t i = start; i < position; ++i) {
      if (first ? !base::IsAsciiAlpha(value[i])
                : !base::IsAsciiAlphaNumeric(value[i])) {
        return false;
      }
    }
    first = false;
    if (position < value.size()) {
      ++position;
      if (position == value.size()) {
        return false;
      }
    }
  }
  return true;
}

// :lang() without CSSLangExtendedRanges, which Chrome's stable set lacks:
// the language starts with the argument, in any ASCII case, followed by its
// end or "-".
bool MatchesLang(const Element& element, const CssSimpleSelector& selector) {
  const std::u16string language =
      pseudo_class_state::InheritedLanguage(element);
  if (!language.empty() && !IsValidLanguageTag(language)) {
    return false;
  }
  for (const std::u16string& argument : selector.arguments) {
    if (language.empty() || language.size() < argument.size() ||
        !base::EqualsCaseInsensitiveASCII(
            std::u16string_view(language).substr(0, argument.size()),
            argument)) {
      continue;
    }
    if (language.size() == argument.size() ||
        language[argument.size()] == '-') {
      return true;
    }
  }
  return false;
}

bool MatchesDir(const Element& element, const CssSimpleSelector& selector) {
  if (selector.arguments.empty()) {
    return false;
  }
  const std::u16string& argument = selector.arguments.front();
  if (base::EqualsCaseInsensitiveASCII(argument, u"ltr")) {
    return !pseudo_class_state::IsRightToLeft(element);
  }
  if (base::EqualsCaseInsensitiveASCII(argument, u"rtl")) {
    return pseudo_class_state::IsRightToLeft(element);
  }
  return false;
}

}  // namespace

SelectorChecker::SelectorChecker(const Node* scope) : scope_(scope) {}

bool SelectorChecker::MatchesList(const CssSelectorList& list,
                                  const Element& element) const {
  return std::any_of(list.complexes.begin(), list.complexes.end(),
                     [this, &element](const CssComplexSelector& complex) {
                       return Matches(complex, element);
                     });
}

bool SelectorChecker::Matches(const CssComplexSelector& complex,
                              const Element& element) const {
  if (complex.compounds.empty()) {
    return false;
  }
  return MatchSelector({&complex, complex.compounds.size() - 1, &element,
                        nullptr}) == MatchStatus::kMatches;
}

SelectorChecker::MatchStatus SelectorChecker::MatchSelector(
    const Context& context) const {
  const CssCompoundSelector& compound =
      context.complex->compounds[context.index];
  if (!MatchesCompound(compound, context)) {
    return MatchStatus::kFailsLocally;
  }
  if (context.index == 0) {
    return MatchStatus::kMatches;
  }
  return MatchForRelation(context);
}

SelectorChecker::MatchStatus SelectorChecker::MatchForRelation(
    const Context& context) const {
  Context next = context;
  next.index = context.index - 1;
  switch (context.complex->compounds[context.index].relation) {
    case CssRelation::kDescendant:
    case CssRelation::kRelativeDescendant:
      for (next.element = context.element->ParentElement(); next.element;
           next.element = next.element->ParentElement()) {
        const MatchStatus status = MatchSelector(next);
        if (status == MatchStatus::kMatches ||
            status == MatchStatus::kFailsCompletely) {
          return status;
        }
      }
      return MatchStatus::kFailsCompletely;
    case CssRelation::kChild:
    case CssRelation::kRelativeChild: {
      next.element = context.element->ParentElement();
      if (!next.element) {
        return MatchStatus::kFailsCompletely;
      }
      const MatchStatus status = MatchSelector(next);
      // If .b's parent is not .a in ".a > .b ~ .c", no other sibling of .c
      // has a parent that is.
      return status == MatchStatus::kFailsLocally
                 ? MatchStatus::kFailsAllSiblings
                 : status;
    }
    case CssRelation::kDirectAdjacent:
    case CssRelation::kRelativeDirectAdjacent:
      next.element = context.element->PreviousElementSibling();
      if (!next.element) {
        return MatchStatus::kFailsAllSiblings;
      }
      return MatchSelector(next);
    case CssRelation::kIndirectAdjacent:
    case CssRelation::kRelativeIndirectAdjacent:
      for (next.element = context.element->PreviousElementSibling();
           next.element;
           next.element = next.element->PreviousElementSibling()) {
        const MatchStatus status = MatchSelector(next);
        if (status != MatchStatus::kFailsLocally) {
          return status;
        }
      }
      return MatchStatus::kFailsAllSiblings;
    case CssRelation::kNone:
      return MatchStatus::kFailsCompletely;
  }
  NOTREACHED();
}

bool SelectorChecker::MatchesCompound(const CssCompoundSelector& compound,
                                      const Context& context) const {
  return std::all_of(compound.simples.begin(), compound.simples.end(),
                     [this, &context](const CssSimpleSelector& simple) {
                       return CheckOne(simple, context);
                     });
}

bool SelectorChecker::CheckOne(const CssSimpleSelector& selector,
                               const Context& context) const {
  const Element& element = *context.element;
  switch (selector.match) {
    case Match::kTag:
      return MatchesTag(selector, element);
    case Match::kId: {
      const std::u16string* id = element.FindAttribute(u"id");
      if (!id) {
        return false;
      }
      return element.GetDocument().InQuirksMode()
                 ? base::ToLowerASCII(*id) == selector.name
                 : *id == selector.name;
    }
    case Match::kClass: {
      const std::u16string* classes = element.FindAttribute(u"class");
      return classes && ClassListContains(*classes, selector.name,
                                          element.GetDocument().InQuirksMode());
    }
    case Match::kAttributeSet:
    case Match::kAttributeExact:
    case Match::kAttributeList:
    case Match::kAttributeHyphen:
    case Match::kAttributeBegin:
    case Match::kAttributeEnd:
    case Match::kAttributeContain:
      return AnyAttributeMatches(selector, element);
    case Match::kPseudoClass:
      return CheckPseudoClass(selector, context);
    case Match::kPseudoElement:
      // Blink's SelectorQuery leaves out a selector with a pseudo-element,
      // and no element of the tree is one.
      return false;
  }
  NOTREACHED();
}

bool SelectorChecker::MatchesAnyInList(const CssSelectorList& list,
                                       const Context& context) const {
  for (const CssComplexSelector& complex : list.complexes) {
    if (complex.compounds.empty()) {
      continue;
    }
    if (MatchSelector({&complex, complex.compounds.size() - 1, context.element,
                       context.relative_anchor}) == MatchStatus::kMatches) {
      return true;
    }
  }
  return false;
}

bool SelectorChecker::CheckPseudoScope(const Element& element) const {
  if (!scope_) {
    return false;
  }
  if (scope_->IsElementNode()) {
    return scope_ == &element;
  }
  return &element == element.GetDocument().DocumentElement();
}

// Blink's CheckPseudoHas(): whether an element that a relative selector of
// the list matches lies where its first relation says from `anchor`: below
// it for a descendant or child relation, and among its later siblings, or
// below them, for a sibling relation.
bool SelectorChecker::CheckPseudoHas(const CssSimpleSelector& selector,
                                     const Element& anchor) const {
  for (const CssComplexSelector& complex : selector.selector_list->complexes) {
    if (complex.compounds.size() < 2) {
      continue;
    }
    const CssRelation first = complex.compounds[1].relation;
    const bool below = first == CssRelation::kRelativeDescendant ||
                       first == CssRelation::kRelativeChild;
    const auto matches = [&](const Element& candidate) {
      return MatchSelector({&complex, complex.compounds.size() - 1, &candidate,
                            &anchor}) == MatchStatus::kMatches;
    };
    const auto any_in_subtree = [&](const Element& root, bool with_root) {
      if (with_root && matches(root)) {
        return true;
      }
      for (const Node* node = root.FirstChild(); node;
           node = node->NextInTree(&root)) {
        if (node->IsElementNode() &&
            matches(static_cast<const Element&>(*node))) {
          return true;
        }
      }
      return false;
    };
    if (below) {
      if (any_in_subtree(anchor, /*with_root=*/false)) {
        return true;
      }
      continue;
    }
    // Past a sibling relation, a later relation may reach below a sibling.
    const bool reaches_below =
        std::any_of(complex.compounds.begin() + 2, complex.compounds.end(),
                    [](const CssCompoundSelector& compound) {
                      return compound.relation == CssRelation::kDescendant ||
                             compound.relation == CssRelation::kChild;
                    });
    for (const Element* sibling = anchor.NextElementSibling(); sibling;
         sibling = sibling->NextElementSibling()) {
      if (reaches_below ? any_in_subtree(*sibling, /*with_root=*/true)
                        : matches(*sibling)) {
        return true;
      }
    }
  }
  return false;
}

unsigned SelectorChecker::NthIndex(const Element& element,
                                   const CssSelectorList* filter,
                                   bool of_type,
                                   bool from_end,
                                   const Context& context) const {
  if (!element.ParentNode()) {
    return 1;
  }
  unsigned index = 1;
  for (const Element* sibling = from_end ? element.NextElementSibling()
                                         : element.PreviousElementSibling();
       sibling; sibling = from_end ? sibling->NextElementSibling()
                                   : sibling->PreviousElementSibling()) {
    bool counts = true;
    if (of_type) {
      counts = HasSameTag(*sibling, element);
    } else if (filter) {
      Context sibling_context = context;
      sibling_context.element = sibling;
      counts = MatchesAnyInList(*filter, sibling_context);
    }
    if (counts) {
      ++index;
    }
  }
  return index;
}

bool SelectorChecker::CheckPseudoClass(const CssSimpleSelector& selector,
                                       const Context& context) const {
  const Element& element = *context.element;
  namespace state = pseudo_class_state;
  switch (selector.pseudo_type) {
    case Pseudo::kNot:
      return !MatchesAnyInList(*selector.selector_list, context);
    case Pseudo::kIs:
    case Pseudo::kWhere:
    case Pseudo::kAny:
      return MatchesAnyInList(*selector.selector_list, context);
    case Pseudo::kHas:
      return CheckPseudoHas(selector, element);
    case Pseudo::kRelativeAnchor:
      return context.relative_anchor == &element;
    case Pseudo::kScope:
    case Pseudo::kParent:
      return CheckPseudoScope(element);
    case Pseudo::kEmpty:
      return IsEmpty(element);
    case Pseudo::kFirstChild:
      return !element.PreviousElementSibling();
    case Pseudo::kLastChild:
      return !element.NextElementSibling();
    case Pseudo::kOnlyChild:
      return !element.PreviousElementSibling() && !element.NextElementSibling();
    case Pseudo::kFirstOfType:
      return NthIndex(element, nullptr, /*of_type=*/true, /*from_end=*/false,
                      context) == 1;
    case Pseudo::kLastOfType:
      return NthIndex(element, nullptr, /*of_type=*/true, /*from_end=*/true,
                      context) == 1;
    case Pseudo::kOnlyOfType:
      return NthIndex(element, nullptr, /*of_type=*/true, /*from_end=*/false,
                      context) == 1 &&
             NthIndex(element, nullptr, /*of_type=*/true, /*from_end=*/true,
                      context) == 1;
    case Pseudo::kNthChild:
    case Pseudo::kNthLastChild: {
      const CssSelectorList* filter = selector.selector_list.get();
      // With "of", the element itself must match the list.
      if (filter && !MatchesAnyInList(*filter, context)) {
        return false;
      }
      return MatchNth(
          selector.nth_a, selector.nth_b,
          NthIndex(element, filter, /*of_type=*/false,
                   selector.pseudo_type == Pseudo::kNthLastChild, context));
    }
    case Pseudo::kNthOfType:
    case Pseudo::kNthLastOfType:
      return MatchNth(
          selector.nth_a, selector.nth_b,
          NthIndex(element, nullptr, /*of_type=*/true,
                   selector.pseudo_type == Pseudo::kNthLastOfType, context));
    case Pseudo::kRoot:
      return &element == element.GetDocument().DocumentElement();
    case Pseudo::kAnyLink:
    case Pseudo::kWebkitAnyLink:
    case Pseudo::kLink:
      return state::IsLink(element);
    case Pseudo::kVisited:
      // A Page keeps no history of visited links.
      return false;
    case Pseudo::kEnabled:
      return state::MatchesEnabled(element);
    case Pseudo::kDisabled:
      return state::MatchesDisabled(element);
    case Pseudo::kChecked:
      return state::IsChecked(element);
    case Pseudo::kDefault:
      return state::IsDefault(element);
    case Pseudo::kIndeterminate:
      return state::IsIndeterminate(element);
    case Pseudo::kRequired:
      return state::IsRequired(element);
    case Pseudo::kOptional:
      return state::IsOptional(element);
    case Pseudo::kReadWrite:
      return state::IsReadWrite(element);
    case Pseudo::kReadOnly:
      return state::IsReadOnly(element);
    case Pseudo::kPlaceholderShown:
      return state::IsPlaceholderShown(element);
    case Pseudo::kValid:
      return state::IsValid(element);
    case Pseudo::kInvalid:
      return state::IsInvalid(element);
    case Pseudo::kInRange:
      return state::IsInRange(element);
    case Pseudo::kOutOfRange:
      return state::IsOutOfRange(element);
    case Pseudo::kDefined:
      return state::IsDefined(element);
    case Pseudo::kOpen:
      return state::IsOpen(element);
    case Pseudo::kModal:
      return state::IsModal(element);
    case Pseudo::kTextField:
      return state::IsTextField(element);
    case Pseudo::kLang:
      return MatchesLang(element, selector);
    case Pseudo::kDir:
      return MatchesDir(element, selector);
    case Pseudo::kHost:
    case Pseudo::kHostContext:
      // A Page's tree has no shadow host.
      return false;
    default:
      // What needs input, focus, style, layout, media, a fullscreen element,
      // a popover shown, a navigation, a view transition, autofill or custom
      // element state, none of which a Page has: :hover, :active, :focus,
      // :focus-visible, :focus-within, :target, :autofill, :fullscreen,
      // :popover-open, :picture-in-picture, :state() and the rest, as Chrome
      // answers them for a page without them.
      return false;
  }
}

}  // namespace mitto
