// A first tree builder: the tree construction stage of the HTML Standard's
// parser, on the HTML tokenizer, for simple documents.
// https://html.spec.whatwg.org/multipage/parsing.html#tree-construction
//
// It has the insertion modes of a document's head and body, and of the text of
// a script, a style or a title, so that html, head and body are made where a
// document leaves them out, and elements, text, comments and the doctype go
// where the standard puts them, with the doctype's quirks mode, the end tags
// that elements such as p and li imply, and void and raw text elements. It
// parses fragments for innerHTML and outerHTML too.
//
// What it lacks is M8's: tables, forms' fine points, templates, framesets,
// select, foreign content, processing instructions, and the list of active
// formatting elements with the adoption agency algorithm. A document that
// needs one still gets a tree, which may differ from Chrome's, and the builder
// records each rule it skipped, which tests read to tell a skipped rule from a
// wrong one.
//
// Upstream: third_party/blink/renderer/core/html/parser/html_tree_builder.h
// Upstream: third_party/blink/renderer/core/html/parser/html_tree_builder.cc
// Upstream: third_party/blink/renderer/core/html/parser/html_construction_site.cc
// Upstream: third_party/blink/renderer/core/html/parser/html_element_stack.cc

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

  // The rules the tree builder skipped so far, each once, in the order it
  // first skipped them.
  const std::vector<std::string>& skipped_rules() const;

 private:
  std::unique_ptr<HtmlTokenizer> tokenizer_;
  std::unique_ptr<HtmlTreeBuilder> tree_builder_;
};

// Parses all of `markup` into `document`, which must have no children, and
// returns the rules the tree builder skipped.
std::vector<std::string> ParseHtmlDocument(Document& document,
                                           std::u16string_view markup);

// The HTML Standard's fragment parsing algorithm for `context`, which
// innerHTML and outerHTML use: the nodes that `markup` makes, with no parent.
// `skipped_rules`, if not null, gets the rules the tree builder skipped.
NodeVector ParseHtmlFragment(Element& context,
                             std::u16string_view markup,
                             std::vector<std::string>* skipped_rules = nullptr);

}  // namespace mitto

#endif  // MITTO_DOM_HTML_TREE_BUILDER_H_
