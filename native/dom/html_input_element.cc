#include "mitto/dom/html_input_element.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/notreached.h"
#include "base/strings/string_util.h"
#include "mitto/bindings/known_gap.h"
#include "mitto/bindings/v8_html_input_element.h"
#include "mitto/dom/event.h"
#include "mitto/dom/event_dispatcher.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-value.h"

namespace mitto {

namespace {

// What a click's legacy-pre-activation behaviour hands to the rest of it, as
// Blink's ClickHandlingState: the checkedness and the indeterminate flag
// before it, and the radio button of the group that was checked.
class ClickHandlingState final : public EventDispatchHandlingState {
 public:
  void Trace(cppgc::Visitor* visitor) const override {
    EventDispatchHandlingState::Trace(visitor);
    visitor->Trace(checked_radio_button);
  }

  bool checked = false;
  bool indeterminate = false;
  cppgc::Member<HTMLInputElement> checked_radio_button;
};

// Fires a trusted event that bubbles, as Blink's Event::CreateBubble() and
// DispatchScopedEvent() do.
void FireBubblingEvent(Node& target,
                       const std::u16string& type,
                       bool composed) {
  bindings::EventInit init;
  init.bubbles = true;
  init.composed = composed;
  const cppgc::Persistent<Event> event(Event::Create(type, init));
  event->SetTrusted(true);
  target.DispatchEvent(event.Get());
}

}  // namespace

// static
HTMLInputElement* HTMLInputElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLInputElement>(document);
}

HTMLInputElement::HTMLInputElement(Document& document)
    : HTMLElement(document, u"input") {}

const bindings::WrapperTypeInfo* HTMLInputElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLInputElement::kWrapperTypeInfo;
}

bool HTMLInputElement::Checked() const {
  if (!dirty_checkedness_) {
    return HasAttribute(u"checked");
  }
  return is_checked_;
}

void HTMLInputElement::SetChecked(bool checked) {
  // Blink's HTMLInputElement::SetChecked(): dirty, also when it stays.
  const bool was_checked = Checked();
  dirty_checkedness_ = true;
  is_checked_ = checked;
  if (was_checked == checked || !checked ||
      GetCheckableType() != CheckableType::kRadio) {
    return;
  }
  // RadioButtonGroupScope::UpdateCheckedState(): the others of its group
  // are unchecked.
  for (HTMLInputElement* other : RadioButtonGroup()) {
    if (other != this && other->Checked()) {
      other->SetChecked(false);
    }
  }
}

std::u16string HTMLInputElement::Type() const {
  // Blink's InputType::NormalizeTypeName().
  static constexpr std::u16string_view kTypes[] = {
      u"button", u"checkbox", u"color",  u"date",  u"datetime-local",
      u"email",  u"file",     u"hidden", u"image", u"month",
      u"number", u"password", u"radio",  u"range", u"reset",
      u"search", u"submit",   u"tel",    u"time",  u"url",
      u"week",
  };
  const std::optional<std::u16string> type = GetAttribute(u"type");
  if (!type) {
    return u"text";
  }
  const std::u16string lower = base::ToLowerASCII(*type);
  for (const std::u16string_view known : kTypes) {
    if (lower == known) {
      return lower;
    }
  }
  return u"text";
}

void HTMLInputElement::SetType(const std::u16string& type) {
  SetAttributeWithoutValidation(u"type", type);
}

HTMLInputElement::CheckableType HTMLInputElement::GetCheckableType() const {
  const std::u16string type = Type();
  if (type == u"checkbox") {
    return CheckableType::kCheckbox;
  }
  if (type == u"radio") {
    return CheckableType::kRadio;
  }
  return CheckableType::kOther;
}

Element* HTMLInputElement::FormOwner() const {
  for (Element* ancestor = ParentElement(); ancestor;
       ancestor = ancestor->ParentElement()) {
    if (ancestor->HasHTMLTagName(u"form")) {
      return ancestor;
    }
  }
  return nullptr;
}

std::vector<HTMLInputElement*> HTMLInputElement::RadioButtonGroup() {
  std::vector<HTMLInputElement*> group;
  const std::optional<std::u16string> name = GetAttribute(u"name");
  if (!name || name->empty()) {
    group.push_back(this);
    return group;
  }
  Element* const form = FormOwner();
  Node& root = TreeRoot();
  for (Node* node = &root; node; node = node->NextInTree(&root)) {
    if (!node->IsElementNode() ||
        !static_cast<Element*>(node)->HasHTMLTagName(u"input")) {
      continue;
    }
    auto* const input = static_cast<HTMLInputElement*>(node);
    if (input->GetCheckableType() == CheckableType::kRadio &&
        input->GetAttribute(u"name") == name && input->FormOwner() == form) {
      group.push_back(input);
    }
  }
  return group;
}

HTMLInputElement* HTMLInputElement::CheckedRadioButtonForGroup() {
  if (Checked()) {
    return this;
  }
  for (HTMLInputElement* input : RadioButtonGroup()) {
    if (input->Checked()) {
      return input;
    }
  }
  return nullptr;
}

bool HTMLInputElement::HasActivationBehavior() const {
  // Every input element has activation behaviour in Blink, which for most
  // types comes with forms.
  return true;
}

EventDispatchHandlingState* HTMLInputElement::LegacyPreActivationBehavior(
    Event& event) {
  // Blink's: only for a click of a MouseEvent with the primary button.
  if (event.Type() != u"click" || !event.IsMouseEvent()) {
    return nullptr;
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Value> button =
      bindings::EventOfInterfaceAttribute(isolate, event, "button");
  if (!button->IsNumber() || button.As<v8::Number>()->Value() != 0) {
    return nullptr;
  }
  switch (GetCheckableType()) {
    case CheckableType::kCheckbox: {
      // CheckboxInputType::LegacyPreActivationBehavior(): the checkedness
      // turns over and the indeterminate flag goes, for the listeners to see,
      // and a canceled click puts both back.
      auto* const state = bindings::MakeGarbageCollected<ClickHandlingState>();
      state->checked = Checked();
      state->indeterminate = is_indeterminate_;
      SetChecked(!state->checked);
      is_indeterminate_ = false;
      return state;
    }
    case CheckableType::kRadio: {
      // RadioInputType::LegacyPreActivationBehavior(): checked, with the one
      // of the group that was checked kept for a canceled click.
      auto* const state = bindings::MakeGarbageCollected<ClickHandlingState>();
      state->checked = Checked();
      state->checked_radio_button = CheckedRadioButtonForGroup();
      SetChecked(true);
      return state;
    }
    case CheckableType::kOther:
      return nullptr;
  }
  NOTREACHED();
}

void HTMLInputElement::RunActivationBehaviorBeforeDOMActivate(
    Event& event,
    EventDispatchHandlingState* handling_state) {
  if (!handling_state) {
    return;
  }
  const auto& state = static_cast<const ClickHandlingState&>(*handling_state);
  const bool canceled = event.DefaultPrevented() || event.DefaultHandled();
  // The input activation behaviour of the type it has now, which commits or
  // reverts what the click did, and marks it handled, so that no DOMActivate
  // follows.
  switch (GetCheckableType()) {
    case CheckableType::kCheckbox:
      if (canceled) {
        is_indeterminate_ = state.indeterminate;
        SetChecked(state.checked);
      } else {
        DispatchInputAndChangeEventIfNeeded();
      }
      break;
    case CheckableType::kRadio:
      if (canceled) {
        HTMLInputElement* const checked = state.checked_radio_button.Get();
        if (!checked) {
          SetChecked(false);
        } else if (checked->GetCheckableType() == CheckableType::kRadio &&
                   checked->FormOwner() == FormOwner() &&
                   checked->GetAttribute(u"name") == GetAttribute(u"name")) {
          checked->SetChecked(true);
        }
      } else if (state.checked != Checked()) {
        DispatchInputAndChangeEventIfNeeded();
      }
      break;
    case CheckableType::kOther:
      return;
  }
  event.SetDefaultHandled();
}

bool HTMLInputElement::IsDisabledFormControl() const {
  // A disabled fieldset's controls come with forms.
  return HasAttribute(u"disabled");
}

void HTMLInputElement::DispatchInputAndChangeEventIfNeeded() {
  if (!IsConnected()) {
    return;
  }
  // RadioInputType::ShouldSendChangeEventAfterCheckedChanged(): not for a
  // radio button that was unchecked.
  if (GetCheckableType() == CheckableType::kRadio && !Checked()) {
    return;
  }
  const cppgc::Persistent<HTMLInputElement> self(this);
  FireBubblingEvent(*this, u"input", /*composed=*/true);
  FireBubblingEvent(*this, u"change", /*composed=*/false);
}

}  // namespace mitto
