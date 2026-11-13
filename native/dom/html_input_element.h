// The HTML Standard's HTMLInputElement, with the checkedness of a checkbox and
// a radio button and what a click does to them, as Blink's HTMLInputElement,
// CheckboxInputType and RadioInputType have it. The rest of a form control's
// behaviour comes with forms.
//
// Upstream: third_party/blink/renderer/core/html/forms/html_input_element.h
// Upstream: third_party/blink/renderer/core/html/forms/html_input_element.cc
// Upstream: third_party/blink/renderer/core/html/forms/checkbox_input_type.cc
// Upstream: third_party/blink/renderer/core/html/forms/radio_input_type.cc

#ifndef MITTO_DOM_HTML_INPUT_ELEMENT_H_
#define MITTO_DOM_HTML_INPUT_ELEMENT_H_

#include <string>
#include <vector>

#include "mitto/dom/html_element.h"

namespace mitto {

class HTMLInputElement final : public HTMLElement {
 public:
  static HTMLInputElement* Create(Document& document);

  explicit HTMLInputElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface beyond its reflected attributes.
  //
  // The checkedness: until a script or a click sets it, it is whether the
  // element has the checked content attribute, which defaultChecked reflects,
  // and from then on what was set, as Blink's dirty checkedness has it.
  // Checking a radio button unchecks the others of its group.
  bool CheckedForBinding() const { return Checked(); }
  void SetCheckedForBinding(bool checked) { SetChecked(checked); }
  // The type that its type attribute names, in lower case, or "text" for
  // none or one that Chrome does not have; setting it sets the attribute.
  std::u16string Type() const;
  void SetType(const std::u16string& type);
  bool Indeterminate() const { return is_indeterminate_; }
  void SetIndeterminate(bool indeterminate) {
    is_indeterminate_ = indeterminate;
  }

  // Node:
  bool HasActivationBehavior() const override;
  EventDispatchHandlingState* LegacyPreActivationBehavior(
      Event& event) override;
  void RunActivationBehaviorBeforeDOMActivate(
      Event& event,
      EventDispatchHandlingState* state) override;
  bool IsDisabledFormControl() const override;

  // What the bindings and a click read and set.
  bool Checked() const;
  void SetChecked(bool checked);

 private:
  enum class CheckableType { kCheckbox, kRadio, kOther };

  // What its type attribute makes it, as far as checkedness goes.
  CheckableType GetCheckableType() const;
  // The radio buttons of its group, itself among them: those of the same
  // name, not empty, in the same tree and with the same form.
  std::vector<HTMLInputElement*> RadioButtonGroup();
  // Blink's RadioInputType::CheckedRadioButtonForGroup().
  HTMLInputElement* CheckedRadioButtonForGroup();
  // The form element that it is in, which a radio button group shares.
  // Forms' own rules, such as the form attribute, come with forms.
  Element* FormOwner() const;
  // Blink's DispatchInputAndChangeEventIfNeeded(): input, then change, at a
  // connected checkbox or a radio button that is checked.
  void DispatchInputAndChangeEventIfNeeded();

  bool is_checked_ = false;
  bool dirty_checkedness_ = false;
  bool is_indeterminate_ = false;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_INPUT_ELEMENT_H_
