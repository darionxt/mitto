// The tree construction stage of the HTML Standard's parser, on the HTML
// tokenizer, as Blink's HTMLTreeBuilder and HTMLConstructionSite have it:
// https://html.spec.whatwg.org/multipage/parsing.html#tree-construction
//
// It has every insertion mode, foster parenting, the list of active formatting
// elements with the adoption agency algorithm, framesets, and SVG and MathML
// elements, and parses fragments for any context element. Where the DOM lacks
// what a rule needs, such as a DocumentFragment for a template's contents or
// attributes in namespaces, the node goes in as near Chrome's tree as it can,
// and the builder records the rule, which tests read to tell such a rule from
// a wrong tree.
//
// Upstream: third_party/blink/renderer/core/html/parser/html_tree_builder.h
// Upstream: third_party/blink/renderer/core/html/parser/html_tree_builder.cc
// Upstream: third_party/blink/renderer/core/html/parser/html_construction_site.cc
// Upstream: third_party/blink/renderer/core/html/parser/html_element_stack.cc
// Upstream: third_party/blink/renderer/core/html/parser/html_formatting_element_list.cc

#ifndef MITTO_DOM_HTML_TREE_BUILDER_H_
#define MITTO_DOM_HTML_TREE_BUILDER_H_

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "mitto/dom/node.h"

namespace mitto {

class Document;
class Element;
class HTMLScriptElement;
class HtmlTokenizer;
class HtmlTreeBuilder;

// Parses a document's markup into a Document, which must have no children, as
// the markup arrives.
class HtmlDocumentParser {
 public:
  explicit HtmlDocumentParser(Document& document);
  HtmlDocumentParser(const HtmlDocumentParser&) = delete;
  HtmlDocumentParser& operator=(const HtmlDocumentParser&) = delete;
  ~HtmlDocumentParser();

  // Adds markup, in pieces of any size.
  void Append(std::u16string_view markup);
  // Ends the markup.
  void Finish();

  // Builds the tree from the markup so far. Returns the script element whose
  // end tag it reached, where the standard would run the script before it
  // goes on, or null once it needs more markup or has reached the end. Call
  // it again to go on.
  HTMLScriptElement* Pump();

  // The rules the tree builder could not follow so far, each once, in the
  // order it first met them.
  const std::vector<std::string>& skipped_rules() const;

 private:
  std::unique_ptr<HtmlTokenizer> tokenizer_;
  std::unique_ptr<HtmlTreeBuilder> tree_builder_;
};

// Parses all of `markup` into `document`, which must have no children, and
// returns the rules the tree builder could not follow.
std::vector<std::string> ParseHtmlDocument(Document& document,
                                           std::u16string_view markup);

// The HTML Standard's fragment parsing algorithm for `context`, of any
// namespace, which innerHTML, outerHTML and insertAdjacentHTML() use: the nodes
// that `markup` makes, with no parent. `skipped_rules`, if not null, gets the
// rules the tree builder could not follow.
NodeVector ParseHtmlFragment(Element& context,
                             std::u16string_view markup,
                             std::vector<std::string>* skipped_rules = nullptr);

}  // namespace mitto

#endif  // MITTO_DOM_HTML_TREE_BUILDER_H_
