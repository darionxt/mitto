// Builds a document's tree from XML, on the Rust xml crate's reader, as
// Blink's XMLDocumentParserRs does: the parser that Chrome uses for
// DOMParser's XML types, XMLHttpRequest's responseXML and SVG documents
// (XMLRustForNonXslt). Its nodes, attributes, namespaces and errors are
// Blink's, an error stops it with the error block that Blink shows, and it
// gives the parser each script element whose end tag it reaches, as Blink's
// EndElementNs() hands it to the XML parser's script runner.
//
// It reads what it has been given and waits for more, as Blink's reads each
// piece of the body as it arrives: the end of what has come is no error until
// the input has ended.
//
// Upstream: third_party/blink/renderer/core/xml/parser/xml_document_parser_rs.cc
// Upstream: third_party/blink/renderer/core/xml/parser/xml_document_parser_rs.h

#ifndef MITTO_DOM_XML_TREE_BUILDER_H_
#define MITTO_DOM_XML_TREE_BUILDER_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "mitto/dom/markup_position.h"
#include "mitto/dom/xml_dom_builder.h"
#include "mitto/xml/xml_ffi.rs.h"
#include "mitto/xml/xml_ffi_callbacks.h"
#include "third_party/rust/cxx/v1/cxx.h"

namespace mitto {

class Element;
class HtmlTreeBuilderReferences;

class XmlTreeBuilder final : public XmlReader, public xml_ffi::XmlCallbacks {
 public:
  // For the document that `references` holds, which has no children. With
  // `scripts_already_started`, as for a document of DOMParser or of no
  // window, the script elements it makes never run.
  XmlTreeBuilder(HtmlTreeBuilderReferences& references,
                 bool scripts_already_started);
  XmlTreeBuilder(const XmlTreeBuilder&) = delete;
  XmlTreeBuilder& operator=(const XmlTreeBuilder&) = delete;
  ~XmlTreeBuilder() override;

  // Adds text to what the reader has, in pieces of any size.
  void Append(std::u16string_view text) override;
  // Ends the input: an end that is still inside the document is an error.
  void Finish() override;
  bool HasSeenEndOfFile() const override;
  bool SawError() const override;

  // Builds the tree from what the reader has. Returns the script element
  // whose end tag it reached, which the parser processes before it calls this
  // again, or null once it needs more input, has stopped or has reached the
  // end.
  Element* Pump() override;

  // XMLDocumentParserRs::EndInternal()'s part of the tree, once the input has
  // ended and the parser has taken all of it.
  void End() override;

  // Where the reader's last event began, zero-based.
  MarkupPosition TextPosition() const override;

  // xml_ffi::XmlCallbacks:
  void StartDocument(rust::Str version,
                     rust::Str encoding,
                     xml_ffi::StandaloneInfo standalone_info) override;
  void ProcessingInstruction(rust::Str target, rust::Str data) override;
  void StartElementNs(rust::Str local_name,
                      bool has_prefix,
                      rust::Str prefix,
                      bool has_ns,
                      rust::Str ns,
                      xml_ffi::AttributesIterator& attributes,
                      xml_ffi::NamespacesIterator& namespaces) override;
  void EndElementNs(rust::Str local_name,
                    rust::Str prefix,
                    rust::Str ns) override;
  void Characters(rust::Str characters) override;
  void CData(rust::Str data) override;
  void Comment(rust::Str comment) override;
  void DocType(rust::Str name,
               rust::Str public_id,
               rust::Str system_id) override;
  void EndDocument() override;

 private:
  XmlDomBuilder builder_;
  rust::Box<xml_ffi::XmlReadState> read_state_;
  // The script element whose end tag the last event was.
  raw_ptr<Element> ended_script_;
  // An end of the input so far that is still inside the root element, which
  // only the end of the input makes an error (CarryUnbalancedRootElementError).
  struct CarriedError {
    std::string message;
    MarkupPosition position;
  };
  std::optional<CarriedError> carried_error_;
  bool saw_end_document_ = false;
  bool finish_called_ = false;
};

}  // namespace mitto

#endif  // MITTO_DOM_XML_TREE_BUILDER_H_
