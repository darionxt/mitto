#include "mitto/dom/dom_parser.h"

#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_dom_parser.h"
#include "mitto/dom/document.h"
#include "mitto/dom/html_document.h"
#include "mitto/dom/window.h"
#include "mitto/dom/xml_document.h"

namespace mitto {


// static
DOMParser* DOMParser::Create() {
  return bindings::MakeGarbageCollected<DOMParser>(Window::Current());
}

DOMParser::DOMParser(Window& window) : window_(&window) {}

DOMParser::~DOMParser() = default;

const bindings::WrapperTypeInfo* DOMParser::GetWrapperTypeInfo() const {
  return &bindings::V8DOMParser::kWrapperTypeInfo;
}

void DOMParser::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(window_);
  bindings::ScriptWrappable::Trace(visitor);
}

Document* DOMParser::ParseFromString(
    const std::u16string& string,
    const std::u16string& type,
    bindings::ExceptionState& exception_state) {
  // DocumentInit with the window's URL and the window as its execution
  // context, which gives the window's origin, and no frame: an HTML document,
  // or an XML document whose MIME type is the one given, which the parser
  // that the runtime-enabled features pick reads (document_parser.h).
  Document* document = nullptr;
  if (type == u"text/html") {
    document = HTMLDocument::Create();
  } else {
    document = XMLDocument::Create(type);
  }
  document->SetCreator(window_);
  const Document& creator = window_->GetDocument();
  document->SetUrl(creator.UrlForBinding());
  document->SetOrigin(creator.GetOrigin());
  document->SetAllowDeclarativeShadowRoots(false);
  document->SetContent(string);
  return document;
}

}  // namespace mitto
