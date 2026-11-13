// The script element. Documents run their scripts once a Page loads them
// (#183), and the parser tells each script element it makes what the HTML
// Standard's script processing model needs of it.
//
// Upstream: third_party/blink/renderer/core/html/html_script_element.h
// Upstream: third_party/blink/renderer/core/html/html_script_element.cc

#ifndef MITTO_DOM_HTML_SCRIPT_ELEMENT_H_
#define MITTO_DOM_HTML_SCRIPT_ELEMENT_H_

#include <string>

#include "mitto/dom/html_element.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class HTMLScriptElement final : public HTMLElement {
 public:
  static HTMLScriptElement* Create(Document& document);

  explicit HTMLScriptElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  std::u16string Text() const { return ChildTextContent(); }
  void SetText(const std::u16string& text,
               bindings::ExceptionState& exception_state);

  // What the parser tells a script element before it inserts it, as Blink's
  // CreateElementFlags and ScriptLoader keep it.
  //
  // The script is parser-inserted, with `parser_document` as its parser
  // document and no force async.
  void SetCreatedByParser(Document& parser_document);
  // The script never runs: one that the fragment parser makes, or a copy of
  // one that has run.
  void SetAlreadyStarted() { already_started_ = true; }
  // Where the script's text starts in its document, a zero-based line and
  // column, as Blink's TextPosition: the offsets of an inline script's
  // errors.
  void SetStartPosition(int line, int column);

  bool IsParserInserted() const { return parser_inserted_; }
  Document* ParserDocument() const { return parser_document_.Get(); }
  bool AlreadyStarted() const { return already_started_; }
  int StartLine() const { return start_line_; }
  int StartColumn() const { return start_column_; }

 private:
  bool parser_inserted_ = false;
  cppgc::WeakMember<Document> parser_document_;
  bool already_started_ = false;
  int start_line_ = 0;
  int start_column_ = 0;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_SCRIPT_ELEMENT_H_
