#include "mitto/dom/html_script_element.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_script_element.h"
#include "mitto/dom/child_list_mutation_scope.h"
#include "mitto/dom/document.h"
#include "mitto/dom/text.h"

namespace mitto {

// static
HTMLScriptElement* HTMLScriptElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLScriptElement>(document);
}

HTMLScriptElement::HTMLScriptElement(Document& document)
    : HTMLElement(document, u"script") {}

const bindings::WrapperTypeInfo* HTMLScriptElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLScriptElement::kWrapperTypeInfo;
}

void HTMLScriptElement::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(parser_document_);
  HTMLElement::Trace(visitor);
}

void HTMLScriptElement::SetCreatedByParser(Document& parser_document) {
  parser_inserted_ = true;
  parser_document_ = &parser_document;
}

void HTMLScriptElement::SetStartPosition(int line, int column) {
  start_line_ = line;
  start_column_ = column;
}

void HTMLScriptElement::SetText(const std::u16string& text,
                                bindings::ExceptionState& exception_state) {
  // The DOM standard's "string replace all", one record for observers.
  const ChildListMutationScope mutation(*this);
  RemoveChildren();
  if (!text.empty()) {
    ParserAppendChild(mitto::Text::Create(GetDocument(), text));
  }
}

}  // namespace mitto
