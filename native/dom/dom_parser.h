// DOM Parsing's DOMParser, as Blink's: parseFromString() makes a new document
// of the type it is given, at the URL and with the origin of the window that
// made the parser, with no window of its own, so its scripts never run and
// its parser's scripting flag is off.
//
// An HTML document is parsed as document.open(), write() and close() would
// parse it, with declarative shadow roots refused. The XML types are a Known
// Gap until the XML parser comes, and throw as the Known Gap did.
//
// Upstream: third_party/blink/renderer/core/xml/dom_parser.h
// Upstream: third_party/blink/renderer/core/xml/dom_parser.cc
// Upstream: third_party/blink/renderer/core/dom/document.cc

#ifndef MITTO_DOM_DOM_PARSER_H_
#define MITTO_DOM_DOM_PARSER_H_

#include <string>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class Document;
class Window;

class DOMParser final : public bindings::ScriptWrappable {
 public:
  // A parser of the current window, as Blink's takes its ScriptState's.
  static DOMParser* Create();

  explicit DOMParser(Window& window);
  ~DOMParser() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface. `type` is one of SupportedType's strings.
  Document* ParseFromString(const std::u16string& string,
                            const std::u16string& type,
                            bindings::ExceptionState& exception_state);

 private:
  const cppgc::Member<Window> window_;
};

}  // namespace mitto

#endif  // MITTO_DOM_DOM_PARSER_H_
