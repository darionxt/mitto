// The DOM's DOMImplementation: a document's factory of new documents and
// doctypes, which document.implementation gives.
//
// Upstream: third_party/blink/renderer/core/dom/dom_implementation.h
// Upstream: third_party/blink/renderer/core/dom/dom_implementation.cc

#ifndef MITTO_DOM_DOM_IMPLEMENTATION_H_
#define MITTO_DOM_DOM_IMPLEMENTATION_H_

#include <optional>
#include <string>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class Document;
class DocumentType;
class XMLDocument;

class DOMImplementation final : public bindings::ScriptWrappable {
 public:
  static DOMImplementation* Create(Document& document);

  explicit DOMImplementation(Document& document);
  ~DOMImplementation() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  //
  // A doctype of the document, whose name may hold anything but ASCII
  // whitespace, U+0000 and ">", as Blink's checks it.
  DocumentType* CreateDocumentType(const std::u16string& qualified_name,
                                   const std::u16string& public_id,
                                   const std::u16string& system_id,
                                   bindings::ExceptionState& exception_state);
  // A new XML document with `doctype`, and an element of `namespace_uri` and
  // `qualified_name` unless that is empty, as createElementNS() makes it. Its
  // MIME type is "application/xhtml+xml" for the HTML namespace,
  // "image/svg+xml" for SVG's, and "application/xml" for any other.
  XMLDocument* CreateDocument(
      const std::optional<std::u16string>& namespace_uri,
      const std::u16string& qualified_name,
      DocumentType* doctype,
      bindings::ExceptionState& exception_state);
  // A new HTML document with a doctype, html, head and body, and a title in
  // the head when `title` is given.
  Document* CreateHTMLDocument(const std::optional<std::u16string>& title);
  bool HasFeature() const { return true; }

 private:
  const cppgc::Member<Document> document_;
};

}  // namespace mitto

#endif  // MITTO_DOM_DOM_IMPLEMENTATION_H_
