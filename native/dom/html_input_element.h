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
#include "mitto/dom/qualified_name.h"

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
  InsertionNotificationRequest InsertedInto(Node& insertion_point) override;
  void RemovedFrom(Node& insertion_point) override;
  // Element:
  void AttributeChanged(const QualifiedName& name,
                        const std::u16string* old_value,
                        const std::u16string* new_value) override;
  int32_t DefaultTabIndex() const override;

  // What the bindings, a click and the pseudo-classes read and set.
  bool Checked() const;
  void SetChecked(bool checked);
  // Whether it has been a password field since it last stopped being a text
  // field, which turns spellcheck off by default, as Blink's
  // HasBeenPasswordField() has it.
  bool HasBeenPasswordField() const { return has_been_password_field_; }

 protected:
  // Node: a copy keeps the checkedness, whether it is dirty, and the
  // indeterminate flag, as Blink's CloneNonAttributePropertiesFrom() has it.
  Node* CloneWithoutChildren(Document& document) const override;

 private:
  enum class CheckableType { kCheckbox, kRadio, kOther };

  // What its type attribute makes it, as far as checkedness goes.
  CheckableType GetCheckableType() const;
  // The radio buttons of its group, itself among them: those of the same
  // name, not empty, in the same tree and with the same form. For one in a
  // form or a connected tree, only those that have joined the group, as
  // Blink's RadioButtonGroupScope has them, so the radio buttons of an
  // insertion join it one by one in tree order.
  std::vector<HTMLInputElement*> RadioButtonGroup();
  // Blink's RadioInputType::CheckedRadioButtonForGroup().
  HTMLInputElement* CheckedRadioButtonForGroup();
  // The others of its group that are checked get unchecked, as Blink's
  // RadioButtonGroup::SetCheckedButton() and
  // RadioInputType::WillUpdateCheckedness() do once a radio button is checked.
  void UncheckOthersOfRadioButtonGroup();
  // Blink's AddToRadioButtonGroup(): a radio button joins the group of the
  // form it is in or of the tree it is connected to, on being inserted or on
  // a new name or type, and if it is checked, unchecks the one that was
  // checked there. Blink keeps no group for a radio button of neither.
  void AddToRadioButtonGroup();
  // The form element that it is in, which a radio button group shares.
  // Forms' own rules, such as the form attribute, come with forms.
  Element* FormOwner() const;
  // Blink's DispatchInputAndChangeEventIfNeeded(): input, then change, at a
  // connected checkbox or a radio button that is checked.
  void DispatchInputAndChangeEventIfNeeded();
  // Blink's UpdateHasBeenPasswordField(), on a new type: a text field that
  // is or has been a password field.
  void UpdateHasBeenPasswordField();

  bool is_checked_ = false;
  bool dirty_checkedness_ = false;
  bool is_indeterminate_ = false;
  // Whether it has joined the group of its form or its connected tree, and
  // not left both since.
  bool in_radio_button_group_ = false;
  bool has_been_password_field_ = false;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_INPUT_ELEMENT_H_
