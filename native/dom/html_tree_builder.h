// The tree construction stage of the HTML Standard's parser, on the HTML
// tokenizer, as Blink's HTMLTreeBuilder and HTMLConstructionSite have it:
// https://html.spec.whatwg.org/multipage/parsing.html#tree-construction
//
// It has every insertion mode, foster parenting, the list of active formatting
// elements with the adoption agency algorithm, framesets, and SVG and MathML
// elements, templates with their contents, and parses fragments for any
// context element. Where the DOM lacks what a rule needs, such as a
// ProcessingInstruction node, the builder goes on as near Chrome's tree as it
// can and records the rule, which tests read to tell such a rule from a wrong
// tree.
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

#include "mitto/dom/heap_vector.h"
#include "mitto/dom/node.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class Document;
class Element;
class HTMLScriptElement;
class HtmlTokenizer;
class HtmlTreeBuilder;

// What a tree builder refers to: its document and every element that it holds,
// as Blink's HTMLTreeBuilder traces them, for the builder's owner to trace or
// to hold. A parser that a document holds traces these, rather than holding
// them with Persistents, which would keep the document from the collector
// (document_parser.h).
class HtmlTreeBuilderReferences final
    : public cppgc::GarbageCollected<HtmlTreeBuilderReferences> {
 public:
  explicit HtmlTreeBuilderReferences(Document& document);
  ~HtmlTreeBuilderReferences();

  void Trace(cppgc::Visitor* visitor) const;

  Document& document() const { return *document_; }
  // Keeps `element` while the builder may refer to it.
  void Keep(Element* element);
  // Keeps `elements` alone, without allocating on the C++ heap, so that none
  // of them can go meanwhile.
  void KeepOnly(const std::vector<Element*>& elements);
  size_t size() const { return elements_.size(); }

 private:
  cppgc::Member<Document> document_;
  HeapVector<Element> elements_;
};

// Parses a document's markup into a Document, which must have no children, as
// the markup arrives.
class HtmlDocumentParser {
 public:
  // What the tree builder refers to stays in `references`, which outlives
  // this, or, with none, in references that this holds.
  explicit HtmlDocumentParser(Document& document,
                              HtmlTreeBuilderReferences* references = nullptr);
  HtmlDocumentParser(const HtmlDocumentParser&) = delete;
  HtmlDocumentParser& operator=(const HtmlDocumentParser&) = delete;
  ~HtmlDocumentParser();

  // Adds markup, in pieces of any size.
  void Append(std::u16string_view markup);
  // Ends the markup.
  void Finish();
  bool HasSeenEndOfFile() const;

  // The insertion point of document.write(), which the tokenizer keeps
  // (HtmlTokenizer::PushInsertionPoint()): Insert() adds markup there, or at
  // the end of the markup while none is set.
  void PushInsertionPoint();
  void PopInsertionPoint();
  bool HasInsertionPoint() const;
  void Insert(std::u16string_view markup);

  // Builds the tree from the markup so far. Returns the script element whose
  // end tag it reached, where the standard would run the script before it
  // goes on, or null once it needs more markup or has reached the end. Call
  // it again to go on.
  HTMLScriptElement* Pump();

  // The rules the tree builder could not follow so far, each once, in the
  // order it first met them.
  const std::vector<std::string>& skipped_rules() const;

 private:
  const cppgc::Persistent<HtmlTreeBuilderReferences> own_references_;
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
