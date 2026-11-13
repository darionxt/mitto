// The script element, which its ScriptLoader prepares as the HTML Standard
// has it, once the parser reaches its end tag, or once the DOM connects it,
// gives it children or a src.
//
// Upstream: third_party/blink/renderer/core/html/html_script_element.h
// Upstream: third_party/blink/renderer/core/html/html_script_element.cc

#ifndef MITTO_DOM_HTML_SCRIPT_ELEMENT_H_
#define MITTO_DOM_HTML_SCRIPT_ELEMENT_H_

#include <optional>
#include <string>
#include <string_view>

#include "mitto/dom/html_element.h"
#include "v8/include/cppgc/member.h"

namespace mitto {

class ScriptLoader;

class HTMLScriptElement final : public HTMLElement {
 public:
  static HTMLScriptElement* Create(Document& document);

  explicit HTMLScriptElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  std::u16string Src() const;
  void SetSrc(const std::u16string& src,
              bindings::ExceptionState& exception_state);
  bool Async() const;
  void SetAsync(bool async);
  std::u16string Text() const { return ChildTextContent(); }
  void SetText(const std::u16string& text,
               bindings::ExceptionState& exception_state);
  std::optional<std::u16string> ScriptTextContentForBinding() const;
  void SetScriptTextContentForBinding(
      const std::optional<std::u16string>& value,
      bindings::ExceptionState& exception_state);
  // HTMLScriptElement.supports(): the script types Chrome runs.
  static bool Supports(const std::u16string& type);

  // What the DOM, the parser and the Page use.
  ScriptLoader& Loader() const { return *loader_; }
  // HTMLScriptElement::IsPotentiallyRenderBlocking(): blocking="render", or a
  // classic script that the parser inserted without async or defer.
  bool IsPotentiallyRenderBlocking() const;
  // Fire load and error at the element, which do not bubble.
  void DispatchLoadEvent();
  void DispatchErrorEvent();

  // Node's.
  InsertionNotificationRequest InsertedInto(Node& insertion_point) override;
  void DidNotifySubtreeInsertionsToDocument() override;
  void ChildrenChanged(ChildrenChangeType type,
                       Node& child,
                       bool by_parser) override;
  // Element's.
  void AttributeChanged(std::u16string_view name,
                        const std::u16string* old_value,
                        const std::u16string* new_value) override;

 protected:
  Node* CloneWithoutChildren(Document& document) const override;

 private:
  cppgc::Member<ScriptLoader> loader_;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_SCRIPT_ELEMENT_H_
