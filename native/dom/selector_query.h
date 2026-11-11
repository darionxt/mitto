// The selectors of querySelector(), matches() and closest(), for simple
// selectors: type and universal selectors, IDs, classes and attribute
// selectors, compounds of them, the four combinators and lists.
//
// A selector that is not valid throws a SyntaxError, with Blink's message. A
// valid one that needs more, such as a pseudo-class or a namespace prefix,
// throws a Known Gap's TypeError until the Runtime has style (roadmap, M9).
// Matching follows Blink's SelectorChecker: names of HTML elements and their
// attributes in an HTML document match in any case, IDs and classes do in a
// document in quirks mode, and so do the values of the attributes that the
// HTML Standard lists.
//
// Upstream: third_party/blink/renderer/core/css/parser/css_selector_parser.cc
// Upstream: third_party/blink/renderer/core/css/parser/css_tokenizer.cc
// Upstream: third_party/blink/renderer/core/css/selector_checker.cc
// Upstream: third_party/blink/renderer/core/dom/selector_query.cc

#ifndef MITTO_DOM_SELECTOR_QUERY_H_
#define MITTO_DOM_SELECTOR_QUERY_H_

#include <optional>
#include <string>
#include <vector>

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class Element;
class Node;

class SelectorQuery {
 public:
  // Parses `selectors`, or throws and returns nullopt.
  static std::optional<SelectorQuery> Parse(
      const std::u16string& selectors,
      bindings::ExceptionState& exception_state);

  SelectorQuery(SelectorQuery&&);
  SelectorQuery& operator=(SelectorQuery&&);
  ~SelectorQuery();

  bool Matches(const Element& element) const;
  // The first element inside `root`, in tree order, that matches.
  Element* QueryFirst(const Node& root) const;

  struct Simple {
    enum class Kind {
      kType,
      kUniversal,
      kId,
      kClass,
      kAttributeExists,
      kAttributeEquals,
      kAttributeListContains,
      kAttributeDashMatch,
      kAttributeBegins,
      kAttributeEnds,
      kAttributeContains,
    };
    // How an attribute selector compares values: as the attribute says, or
    // in any case for the flag i.
    enum class Case {
      kDefault,
      kInsensitive,
    };

    Kind kind;
    std::u16string name;
    std::u16string value;
    Case value_case = Case::kDefault;
  };

  // How a compound relates to the compound before it.
  enum class Combinator {
    kNone,
    kDescendant,
    kChild,
    kNextSibling,
    kSubsequentSibling,
  };

  struct Compound {
    Combinator combinator = Combinator::kNone;
    std::vector<Simple> simples;
  };

  // The compounds of one complex selector, from left to right.
  using Complex = std::vector<Compound>;

 private:
  explicit SelectorQuery(std::vector<Complex> list);

  std::vector<Complex> list_;
};

}  // namespace mitto

#endif  // MITTO_DOM_SELECTOR_QUERY_H_
