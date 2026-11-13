// Matches an element against the selectors that the selector parser makes,
// as Blink's SelectorChecker matches them for querySelector(), matches() and
// closest(), in its mode for queries.
//
// It matches each complex selector from right to left, with Blink's statuses
// that stop a search early: a compound that fails for an element fails for
// its siblings too past a child combinator, and everything fails once no
// ancestor matches past a descendant combinator. The pseudo-classes that
// read an element's state, such as :checked or :dir(), are answered by
// pseudo_class_state.h, and those that need style, layout, focus or input,
// such as :hover, give what Chrome gives a page that has none of them. A
// pseudo-element matches no element.
//
// Upstream: third_party/blink/renderer/core/css/selector_checker.cc
// Upstream: third_party/blink/renderer/core/css/check_pseudo_has_argument_context.cc
// Upstream: third_party/blink/renderer/core/dom/nth_index_cache.cc

#ifndef MITTO_DOM_SELECTOR_CHECKER_H_
#define MITTO_DOM_SELECTOR_CHECKER_H_

#include "base/memory/raw_ptr.h"
#include "mitto/css/css_selector.h"

namespace mitto {

class Element;
class Node;

class SelectorChecker {
 public:
  // `scope` is what :scope and & match: the element, or for a document the
  // document element. It is the node that querySelector() was called on, or
  // the element of matches() and closest().
  explicit SelectorChecker(const Node* scope);

  // Whether `element` matches a selector of `list`.
  bool MatchesList(const CssSelectorList& list, const Element& element) const;
  // Whether `element` matches `complex`.
  bool Matches(const CssComplexSelector& complex, const Element& element) const;

 private:
  enum class MatchStatus {
    kMatches,
    kFailsLocally,
    kFailsAllSiblings,
    kFailsCompletely,
  };

  struct Context {
    raw_ptr<const CssComplexSelector> complex;
    // The compound to match `element` against, counted from the left.
    size_t index;
    raw_ptr<const Element> element;
    // The anchor of the :has() whose argument this is, or null.
    raw_ptr<const Element> relative_anchor;
  };

  MatchStatus MatchSelector(const Context& context) const;
  MatchStatus MatchForRelation(const Context& context) const;
  bool MatchesCompound(const CssCompoundSelector& compound,
                       const Context& context) const;
  bool CheckOne(const CssSimpleSelector& selector,
                const Context& context) const;
  bool CheckPseudoClass(const CssSimpleSelector& selector,
                        const Context& context) const;
  bool MatchesAnyInList(const CssSelectorList& list,
                        const Context& context) const;
  bool CheckPseudoHas(const CssSimpleSelector& selector,
                      const Element& anchor) const;
  bool CheckPseudoScope(const Element& element) const;
  // The position of `element` among its element siblings, from the first or
  // the last, counting only those that `filter` matches, if it is given, or
  // those of its type.
  unsigned NthIndex(const Element& element,
                    const CssSelectorList* filter,
                    bool of_type,
                    bool from_end,
                    const Context& context) const;

  const raw_ptr<const Node> scope_;
};

}  // namespace mitto

#endif  // MITTO_DOM_SELECTOR_CHECKER_H_
