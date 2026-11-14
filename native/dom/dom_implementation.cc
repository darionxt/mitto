#include "mitto/dom/dom_implementation.h"

#include <algorithm>

#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_dom_implementation.h"
#include "mitto/dom/document.h"
#include "mitto/dom/document_type.h"
#include "mitto/dom/element.h"
#include "mitto/dom/html_document.h"
#include "mitto/dom/html_element.h"
#include "mitto/dom/html_element_factory.h"
#include "mitto/dom/html_names.h"
#include "mitto/dom/html_parser_idioms.h"
#include "mitto/dom/text.h"
#include "mitto/dom/xml_document.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

namespace {

// Blink's IsValidDoctypeName(): no ASCII whitespace, U+0000 or ">".
bool IsValidDoctypeName(const std::u16string& name) {
  return std::ranges::none_of(
      name, [](char16_t c) { return !c || c == '>' || IsHTMLSpace(c); });
}

}  // namespace

// static
DOMImplementation* DOMImplementation::Create(Document& document) {
  return bindings::MakeGarbageCollected<DOMImplementation>(document);
}

DOMImplementation::DOMImplementation(Document& document)
    : document_(&document) {}

DOMImplementation::~DOMImplementation() = default;

const bindings::WrapperTypeInfo* DOMImplementation::GetWrapperTypeInfo() const {
  return &bindings::V8DOMImplementation::kWrapperTypeInfo;
}

void DOMImplementation::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(document_);
}

DocumentType* DOMImplementation::CreateDocumentType(
    const std::u16string& qualified_name,
    const std::u16string& public_id,
    const std::u16string& system_id,
    bindings::ExceptionState& exception_state) {
  if (!IsValidDoctypeName(qualified_name)) {
    exception_state.ThrowDOMException(
        "InvalidCharacterError",
        base::StrCat({"The provided doctype name ('",
                      base::UTF16ToUTF8(qualified_name),
                      "') contains an invalid character."}));
    return nullptr;
  }
  return DocumentType::Create(*document_, qualified_name, public_id, system_id);
}

XMLDocument* DOMImplementation::CreateDocument(
    const std::optional<std::u16string>& namespace_uri,
    const std::u16string& qualified_name,
    DocumentType* doctype,
    bindings::ExceptionState& exception_state) {
  XMLDocument* document = nullptr;
  if (namespace_uri == html_names::kSvgNamespace) {
    document = XMLDocument::Create(u"image/svg+xml");
  } else if (namespace_uri == html_names::kXhtmlNamespace) {
    document = XMLDocument::Create(u"application/xhtml+xml");
  } else {
    document = XMLDocument::Create(u"application/xml");
  }
  Element* document_element = nullptr;
  if (!qualified_name.empty()) {
    document_element = document->CreateElementNS(namespace_uri, qualified_name,
                                                 exception_state);
    if (exception_state.HadException()) {
      return nullptr;
    }
  }
  if (doctype) {
    document->AppendChild(doctype, exception_state);
  }
  if (document_element) {
    document->AppendChild(document_element, exception_state);
  }
  return document;
}

Document* DOMImplementation::CreateHTMLDocument(
    const std::optional<std::u16string>& title) {
  HTMLDocument* const document = HTMLDocument::Create();
  bindings::ExceptionState exception_state(v8::Isolate::GetCurrent());
  HTMLElement* const html = CreateHTMLElement(*document, u"html");
  HTMLElement* const head = CreateHTMLElement(*document, u"head");
  html->AppendChild(head, exception_state);
  if (title) {
    HTMLElement* const title_element = CreateHTMLElement(*document, u"title");
    title_element->AppendChild(document->CreateTextNode(*title),
                               exception_state);
    head->AppendChild(title_element, exception_state);
  }
  html->AppendChild(CreateHTMLElement(*document, u"body"), exception_state);
  document->AppendChild(DocumentType::Create(*document, u"html", u"", u""),
                        exception_state);
  document->AppendChild(html, exception_state);
  return document;
}

}  // namespace mitto
