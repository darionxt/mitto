// The DOM's HTMLElement: an element of the HTML namespace, and the class of
// those whose name has no interface of its own, such as section or b.
//
// Upstream: third_party/blink/renderer/core/html/html_element.h
// Upstream: third_party/blink/renderer/core/html/html_element.cc

#ifndef MITTO_DOM_HTML_ELEMENT_H_
#define MITTO_DOM_HTML_ELEMENT_H_

#include <optional>
#include <string>

#include "mitto/bindings/dictionaries.h"
#include "mitto/dom/element.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-value.h"

namespace mitto {

class HTMLElement : public Element {
 public:
  static HTMLElement* Create(Document& document,
                             const std::u16string& local_name);

  HTMLElement(Document& document, const std::u16string& local_name);
  ~HTMLElement() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface beyond its reflected attributes.
  std::u16string Autocapitalize() const;
  void SetAutocapitalize(const std::u16string& value);
  // The hidden attribute: false for none, "until-found" for that keyword in
  // any case, and true for any other value, as Blink's hidden() gives it.
  v8::Local<v8::Value> Hidden() const;
  // Null, false, "", 0 and NaN remove the attribute, "until-found" in any case
  // sets that, and any other value sets it empty, as Blink's setHidden() does.
  void SetHidden(
      const std::optional<bindings::BooleanOrUnrestrictedDoubleOrDOMString>&
          value);
  // The dir attribute limited to "ltr", "rtl" and "auto", as Blink's
  // ToValidDirValue() limits it.
  std::u16string Dir() const;
  void SetDir(const std::u16string& value);
  // The translate attribute of this element or the nearest HTML ancestor
  // that says "yes", "" or "no", and true for none.
  bool Translate() const;
  void SetTranslate(bool value);
  // "true", in any case, and a link with href and an image by default.
  virtual bool Draggable() const;
  void SetDraggable(bool value);
  // The spellcheck attribute of this element or the nearest ancestor that
  // says "true", "" or "false", and true for none, as Chrome's default is.
  bool Spellcheck() const;
  void SetSpellcheck(bool value);
  std::u16string ContentEditable() const;
  void SetContentEditable(const std::u16string& value,
                          bindings::ExceptionState& exception_state);
  // Whether the element is editable: its own or the nearest HTML ancestor's
  // contenteditable makes it so, or the designMode of the document it is
  // connected to does.
  bool IsContentEditableForBinding() const;
  // Fires a click at the element, which runs its activation behaviour, as
  // Blink's HTMLElement::click() does for a script.
  void Click() { DispatchSimulatedClick(); }

  // Element's: the summary of its parent details, its first summary child,
  // is focusable, as Blink's HTMLSummaryElement is.
  int32_t DefaultTabIndex() const override;

  // Node's: a summary has activation behaviour, as Blink's
  // HTMLSummaryElement has: a click on the main summary of a details element
  // opens or closes it, unless it was on a form control, a label, an embed or
  // an element with activation behaviour of its own in the summary.
  bool HasActivationBehavior() const override;
  void RunActivationBehavior(Event& event,
                             EventDispatchHandlingState* state) override;

 private:
  // Whether it is a summary that is the first summary child of a details
  // element, as Blink's IsMainSummary() says.
  bool IsMainSummary() const;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_ELEMENT_H_
