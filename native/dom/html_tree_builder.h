// The tree construction stage of the HTML Standard's parser, on the HTML
// tokenizer, as Blink's HTMLTreeBuilder and HTMLConstructionSite have it:
// https://html.spec.whatwg.org/multipage/parsing.html#tree-construction
//
// It has every insertion mode, foster parenting, the list of active formatting
// elements with the adoption agency algorithm, framesets, and SVG and MathML
// elements, templates with their contents, and parses fragments for any
// context element, with the scripting flag on or off, processing instructions,
// and the patching of `<template for>`. Where the DOM lacks what a rule needs,
// such as a declarative shadow root, the builder goes on as near Chrome's tree
// as it can and records the rule, which tests read to tell such a rule from a
// wrong tree.
//
// Upstream: third_party/blink/renderer/core/html/parser/html_tree_builder.h
// Upstream: third_party/blink/renderer/core/html/parser/html_tree_builder.cc
// Upstream: third_party/blink/renderer/core/html/parser/html_construction_site.cc
// Upstream: third_party/blink/renderer/core/html/parser/html_element_stack.cc
// Upstream: third_party/blink/renderer/core/html/parser/html_formatting_element_list.cc
// Upstream: third_party/blink/renderer/core/html/parser/html_parser_options.cc
// Upstream: third_party/blink/renderer/core/html/parser/patch.cc

#ifndef MITTO_DOM_HTML_TREE_BUILDER_H_
#define MITTO_DOM_HTML_TREE_BUILDER_H_

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "mitto/dom/heap_vector.h"
#include "mitto/dom/markup_position.h"
#include "mitto/dom/node.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class Document;
class Element;
class HtmlTokenizer;
class HtmlTreeBuilder;

// What Blink's HTMLParserOptions holds of a parse.
struct HtmlParserOptions {
  // The options for a parse into `document`, as HTMLParserOptions(Document*)
  // gives them: the scripting flag is on for a document that has a window,
  // whose scripts a Page always runs, and off for one of no window, such as
  // createHTMLDocument()'s or a template's contents.
  static HtmlParserOptions ForDocument(const Document& document);

  // The HTML Standard's scripting flag. On, noscript holds raw text. Off, its
  // contents are markup, with the "in head noscript" insertion mode.
  bool scripting_flag = false;
};

// What a tree builder refers to: its document and every node that it holds,
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
  // Keeps `node` while the builder may refer to it.
  void Keep(Node* node);
  // Keeps `nodes` alone, without allocating on the C++ heap, so that none of
  // them can go meanwhile.
  void KeepOnly(const std::vector<Node*>& nodes);
  size_t size() const { return nodes_.size(); }

 private:
  cppgc::Member<Document> document_;
  HeapVector<Node> nodes_;
};

// Parses a document's markup into a Document, which must have no children, as
// the markup arrives.
class HtmlDocumentParser {
 public:
  // What the tree builder refers to stays in `references`, which outlives
  // this, or, with none, in references that this holds. The options are the
  // document's, HtmlParserOptions::ForDocument(), unless given.
  explicit HtmlDocumentParser(Document& document,
                              HtmlTreeBuilderReferences* references = nullptr);
  HtmlDocumentParser(Document& document,
                     const HtmlParserOptions& options,
                     HtmlTreeBuilderReferences* references = nullptr);
  HtmlDocumentParser(const HtmlDocumentParser&) = delete;
  HtmlDocumentParser& operator=(const HtmlDocumentParser&) = delete;
  ~HtmlDocumentParser();

  // Adds markup, in pieces of any size.
  void Append(std::u16string_view markup);
  // Ends the markup.
  void Finish();
  bool HasSeenEndOfFile() const;
  // Where the tokenizer is, as Blink's HTMLDocumentParser::GetTextPosition()
  // gives it.
  MarkupPosition TextPosition() const;

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
  Element* Pump();

  // The rules the tree builder could not follow so far, each once, in the
  // order it first met them.
  const std::vector<std::string>& skipped_rules() const;

 private:
  const cppgc::Persistent<HtmlTreeBuilderReferences> own_references_;
  std::unique_ptr<HtmlTokenizer> tokenizer_;
  std::unique_ptr<HtmlTreeBuilder> tree_builder_;
};

// Parses all of `markup` into `document`, which must have no children, and
// returns the rules the tree builder could not follow. The options are the
// document's, HtmlParserOptions::ForDocument(), unless given.
std::vector<std::string> ParseHtmlDocument(Document& document,
                                           std::u16string_view markup);
std::vector<std::string> ParseHtmlDocument(Document& document,
                                           std::u16string_view markup,
                                           const HtmlParserOptions& options);

// The options of a fragment parse for `context`, as Blink's fragment parser
// gives them: those of the document that the fragment is made in, which for a
// template is the template's contents' document.
HtmlParserOptions FragmentParserOptions(const Element& context);

// What a fragment's script elements are, as Blink's ParserContentPolicy has
// it: the parser's and already started, so that they never run, as innerHTML's
// are, or neither, so that they run once the DOM connects them, as
// createContextualFragment()'s do
// (kAllowScriptingContentAndDoNotMarkAlreadyStarted).
enum class FragmentScripts { kAlreadyStarted, kRunnable };

// The HTML Standard's fragment parsing algorithm for `context`, of any
// namespace, which innerHTML, outerHTML and insertAdjacentHTML() use: the nodes
// that `markup` makes, with no parent. `skipped_rules`, if not null, gets the
// rules the tree builder could not follow. The options are
// FragmentParserOptions(context), unless given, and the scripts already
// started, unless `scripts` says otherwise.
NodeVector ParseHtmlFragment(Element& context,
                             std::u16string_view markup,
                             std::vector<std::string>* skipped_rules = nullptr);
NodeVector ParseHtmlFragment(
    Element& context,
    std::u16string_view markup,
    const HtmlParserOptions& options,
    std::vector<std::string>* skipped_rules = nullptr,
    FragmentScripts scripts = FragmentScripts::kAlreadyStarted);

}  // namespace mitto

#endif  // MITTO_DOM_HTML_TREE_BUILDER_H_
