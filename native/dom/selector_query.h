// The selectors of querySelector(), querySelectorAll(), matches() and
// closest(), as Blink's SelectorQuery runs them: parsed by the selector
// parser in native/css for the document of the node they are called on, and
// matched by SelectorChecker.
//
// An empty selector and one that is not valid throw a SyntaxError with
// Blink's messages. A selector with a pseudo-element is valid and matches
// nothing.
//
// Upstream: third_party/blink/renderer/core/css/selector_query.cc

#ifndef MITTO_DOM_SELECTOR_QUERY_H_
#define MITTO_DOM_SELECTOR_QUERY_H_

#include <memory>
#include <optional>
#include <string>

#include "mitto/dom/node.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

struct CssSelectorList;
class Element;

class SelectorQuery {
 public:
  // Parses `selectors` for the document of `node`, or throws and returns
  // nullopt.
  static std::optional<SelectorQuery> Parse(
      const std::u16string& selectors,
      const Node& node,
      bindings::ExceptionState& exception_state);

  SelectorQuery(SelectorQuery&&);
  SelectorQuery& operator=(SelectorQuery&&);
  ~SelectorQuery();

  // Whether `element` matches, with itself as :scope.
  bool Matches(const Element& element) const;
  // `element` or its closest ancestor that matches, with `element` as :scope.
  Element* Closest(const Element& element) const;
  // The first element inside `root`, in tree order, that matches, with
  // `root` as :scope.
  Element* QueryFirst(const Node& root) const;
  // Each element inside `root` that matches, in tree order.
  NodeVector QueryAll(const Node& root) const;

 private:
  explicit SelectorQuery(std::unique_ptr<CssSelectorList> list);

  std::unique_ptr<CssSelectorList> list_;
};

}  // namespace mitto

#endif  // MITTO_DOM_SELECTOR_QUERY_H_
