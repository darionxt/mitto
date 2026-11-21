// Builds a document's tree, or a fragment's, from XML on libxml2, as Blink's
// XMLDocumentParser drives it: the parser of a navigation's XML, and of
// DOMParser's and SVG documents where XMLRustForNonXslt is off, as every
// Persona's variations seed has it. libxml2's errors are Blink's, from its
// own messages, and leave the parse going until libxml2 stops reading; the
// block of the errors goes in once the input has ended.
//
// libxml2 reads each piece as it comes and calls back what it read. The
// builder keeps those calls in order and plays them back as the parser pumps
// it, as Blink keeps them while a script holds the parser back
// (PendingCallbacks), so a script element whose end tag libxml2 read stops
// the playback until it has run.
//
// External entities and DTDs are never loaded (XMLNoExternalEntities), and
// libxml2 opens no file: its input callbacks give nothing.
//
// Upstream: third_party/blink/renderer/core/xml/parser/xml_document_parser.cc
// Upstream: third_party/blink/renderer/core/xml/parser/xml_document_parser.h

#ifndef MITTO_DOM_LIBXML_TREE_BUILDER_H_
#define MITTO_DOM_LIBXML_TREE_BUILDER_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "base/containers/circular_deque.h"
#include "base/memory/raw_ptr.h"
#include "mitto/dom/markup_position.h"
#include "mitto/dom/node.h"
#include "mitto/dom/xml_dom_builder.h"

typedef struct _xmlParserCtxt xmlParserCtxt;

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class DocumentFragment;
class Element;
class HtmlTreeBuilderReferences;

class LibxmlTreeBuilder final : public XmlReader {
 public:
  // For the document that `references` holds, which has no children. With
  // `scripts_already_started`, as for a document of DOMParser or of no
  // window, the script elements it makes never run.
  LibxmlTreeBuilder(HtmlTreeBuilderReferences& references,
                    bool scripts_already_started);
  LibxmlTreeBuilder(const LibxmlTreeBuilder&) = delete;
  LibxmlTreeBuilder& operator=(const LibxmlTreeBuilder&) = delete;
  ~LibxmlTreeBuilder() override;

  // libxml2 reads `text`, which may be any piece of the input.
  void Append(std::u16string_view text) override;
  // The input has ended: libxml2 reads its end, as Blink's DoEnd() tells it.
  void Finish() override;
  bool HasSeenEndOfFile() const override;

  // Plays back what libxml2 read. Returns the script element whose end tag it
  // reached, which the parser processes before it calls this again, or null
  // once nothing waits.
  Element* Pump() override;

  // XMLDocumentParser::end()'s part of the tree, once the parser has taken
  // all of the input.
  void End() override;

  // Where libxml2 was when it read what plays back now, zero-based.
  MarkupPosition TextPosition() const override;

  // XMLDocumentParser::ParseDocumentFragment(): `source` as the children of
  // `fragment`, whose context is `context_element`, which may be null.
  // Returns whether the markup is well formed enough, as Blink's
  // AppendFragmentSource() judges it, unless an attribute threw, which the
  // caller then throws.
  static bool ParseFragment(std::u16string_view source,
                            DocumentFragment& fragment,
                            Element* context_element,
                            HtmlTreeBuilderReferences& references);

  // What libxml2's callbacks give, which the builder keeps.
  struct Event;
  void Enqueue(std::unique_ptr<Event> event);
  bool is_xhtml_document() const { return is_xhtml_document_; }
  void set_is_xhtml_document() { is_xhtml_document_ = true; }
  // Where libxml2 is now, zero-based.
  MarkupPosition ContextPosition() const;

 private:
  // For a fragment.
  LibxmlTreeBuilder(HtmlTreeBuilderReferences& references,
                    DocumentFragment& fragment,
                    Element* context_element);
  void InitializeParserContext(const std::string* fragment_source);
  void Play(Event& event);

  XmlDomBuilder builder_;
  raw_ptr<xmlParserCtxt> context_ = nullptr;
  base::circular_deque<std::unique_ptr<Event>> events_;
  MarkupPosition position_;
  raw_ptr<Element> ended_script_ = nullptr;
  bool finished_ = false;
  bool is_xhtml_document_ = false;
};

// Blink's CreateFragmentForInnerOuterHTML() for a document that is not HTML:
// the nodes that `markup` makes as XML in `context`'s document, with no
// parent, or none after throwing what an attribute threw or Blink's
// SyntaxError for markup that is not well formed.
NodeVector ParseXmlFragment(Element& context,
                            std::u16string_view markup,
                            bindings::ExceptionState& exception_state);

}  // namespace mitto

#endif  // MITTO_DOM_LIBXML_TREE_BUILDER_H_
