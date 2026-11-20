// What Blink's two XML parsers, XMLDocumentParser on libxml2 and
// XMLDocumentParserRs on the Rust xml crate, do alike with what they read:
// they make the nodes of a document or of a fragment, with Blink's checks of
// attribute names and namespaces, keep text until the next node, stop on a
// fatal error with the block of the errors, and hand on each script element
// whose end tag they reach (xml_tree_builder.h, libxml_tree_builder.h).
//
// Upstream: third_party/blink/renderer/core/xml/parser/xml_document_parser.cc
// Upstream: third_party/blink/renderer/core/xml/parser/xml_document_parser_rs.cc

#ifndef MITTO_DOM_XML_DOM_BUILDER_H_
#define MITTO_DOM_XML_DOM_BUILDER_H_

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ref.h"
#include "mitto/dom/markup_position.h"
#include "mitto/dom/xml_errors.h"

namespace mitto {

class Document;
class Element;
class HtmlTreeBuilderReferences;
class Node;
class Text;

// What a document's parser asks of the tree builder of either XML reader.
class XmlReader {
 public:
  virtual ~XmlReader() = default;
  // Adds a piece of the input, which may be empty.
  virtual void Append(std::u16string_view text) = 0;
  // Ends the input.
  virtual void Finish() = 0;
  virtual bool HasSeenEndOfFile() const = 0;
  // Builds the tree from what has been read. Returns the script element whose
  // end tag it reached, which the parser processes before it calls this
  // again, or null once it needs more input or is done.
  virtual Element* Pump() = 0;
  // The end of the document's parse, once the parser has taken all of the
  // input: the block of the errors, or the text that waits.
  virtual void End() = 0;
  // Where the reader is, zero-based.
  virtual MarkupPosition TextPosition() const = 0;
};

class XmlDomBuilder {
 public:
  // A namespace declaration of a start tag: its prefix, none for the default
  // namespace, and its URI, empty for none.
  struct NamespaceDeclaration {
    std::optional<std::u16string> prefix;
    std::u16string uri;
  };
  // An attribute of a start tag: its qualified name and its namespace, none
  // for none, as the reader resolved them.
  struct ReadAttribute {
    std::optional<std::u16string> namespace_uri;
    std::u16string qualified_name;
    std::u16string value;
  };
  // Blink's StandaloneInfo: what an XML declaration said of standalone.
  enum class Standalone {
    kUnspecified = -2,
    kNoXmlDeclaration = -1,
    kNo = 0,
    kYes = 1,
  };

  // For the document that `references` holds, or, with `fragment`, for that
  // fragment of it, whose context is `context_element`, which may be null.
  // With `scripts_already_started`, as for a document of DOMParser, of no
  // window or a fragment, the script elements it makes never run.
  XmlDomBuilder(HtmlTreeBuilderReferences& references,
                Node* fragment,
                Element* context_element,
                bool scripts_already_started);
  XmlDomBuilder(const XmlDomBuilder&) = delete;
  XmlDomBuilder& operator=(const XmlDomBuilder&) = delete;
  ~XmlDomBuilder();

  Document& document() const;
  bool IsStopped() const { return stopped_; }
  bool saw_error() const { return saw_error_; }
  bool parsing_fragment() const { return fragment_; }

  // An error at a zero-based position. A fatal one stops the parse.
  void HandleError(XmlErrors::Type type,
                   std::string_view message,
                   MarkupPosition where);
  // StopParsing(): the block of the errors goes in, if there were any, and
  // nothing more is built.
  void StopParsing();

  void StartDocument(const std::optional<std::u16string>& version,
                     const std::optional<std::u16string>& encoding,
                     Standalone standalone);
  void ProcessingInstruction(const std::u16string& target,
                             const std::u16string& data);
  // `position` is where the reader is, from which a script element's text
  // begins. In a fragment, an attribute whose name cannot be made stops the
  // parse with the error that the call that parses it throws.
  void StartElement(const std::u16string& local_name,
                    const std::optional<std::u16string>& prefix,
                    const std::optional<std::u16string>& namespace_uri,
                    const std::vector<NamespaceDeclaration>& namespaces,
                    const std::vector<ReadAttribute>& attributes,
                    MarkupPosition position);
  // The script element whose end tag it was, which runs before the parse
  // goes on, or null.
  Element* EndElement();
  void Characters(std::u16string_view text);
  void CData(const std::u16string& data);
  void Comment(const std::u16string& data);
  void DocType(const std::u16string& name,
               const std::u16string& public_id,
               const std::u16string& system_id);
  // The text that waits goes in.
  void EndDocument();
  // The end of a document's parse: the block of errors that did not stop it,
  // or the text that waits.
  void End();

  // Where the fragment's context resolves a prefix of an attribute that the
  // reader did not, and none for one it does not.
  std::optional<std::u16string> FragmentNamespaceOfPrefix(
      const std::u16string& prefix) const;
  // An attribute whose prefix nothing declared: the parse stops, and in a
  // fragment with Blink's NamespaceError, which the call that parses it
  // throws.
  void FailOnUndeclaredAttributePrefix(const std::u16string& prefix,
                                       const std::u16string& local_name);

 private:
  void PushCurrentNode(Node* node, MarkupPosition where);
  void PopCurrentNode();
  void CreateLeafTextNodeIfNeeded();
  bool UpdateLeafTextNode();
  void KeepReferences();

  const raw_ref<HtmlTreeBuilderReferences> references_;
  const raw_ptr<Node> fragment_;
  const bool scripts_already_started_;
  XmlErrors errors_;
  raw_ptr<Node> current_node_;
  std::vector<raw_ptr<Node>> current_node_stack_;
  raw_ptr<Text> leaf_text_node_;
  std::u16string buffered_text_;
  // In a fragment, the element below which a default namespace declaration
  // was undone, and the namespaces that the context had in scope.
  raw_ptr<Node> ancestor_resetting_namespace_;
  std::optional<std::u16string> default_namespace_uri_;
  std::map<std::u16string, std::u16string> prefix_to_namespace_;
  bool saw_error_ = false;
  bool saw_css_ = false;
  bool saw_xsl_ = false;
  bool stopped_ = false;
};

}  // namespace mitto

#endif  // MITTO_DOM_XML_DOM_BUILDER_H_
