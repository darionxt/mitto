// The DOM's HTMLElement: an element of the HTML namespace, and the class of
// those whose name has no interface of its own, such as section or b.
//
// Upstream: third_party/blink/renderer/core/html/html_element.h
// Upstream: third_party/blink/renderer/core/html/html_element.cc

#ifndef MITTO_DOM_HTML_ELEMENT_H_
#define MITTO_DOM_HTML_ELEMENT_H_

#include <optional>
#include <string>
#include <string_view>

#include "mitto/bindings/dictionaries.h"
#include "mitto/dom/element.h"
#include "mitto/dom/popover_data.h"
#include "v8/include/cppgc/member.h"
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
  // The writingsuggestions attribute of this element or the nearest ancestor
  // that has one: "false" for that keyword in any case, else "true", as
  // Blink's writingSuggestions() reads it.
  std::u16string WritingSuggestions() const;
  void SetWritingSuggestions(const std::u16string& value);
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

  // The Popover API, as Blink's togglePopover(), showPopover() and
  // hidePopover() run it: the beforetoggle event, the popover stacks, which
  // showing an auto or hint popover closes down to its nearest open popover
  // ancestor, and the toggle event that a task fires, which a later change
  // folds into itself. A Page has no focus, top layer, close watcher or
  // light dismiss for a popover yet.
  bool TogglePopover(
      const std::optional<bindings::TogglePopoverOptionsOrBoolean>& options,
      bindings::ExceptionState& exception_state);
  void ShowPopover(const std::optional<bindings::ShowPopoverOptions>& options,
                   bindings::ExceptionState& exception_state);
  void HidePopover(bindings::ExceptionState& exception_state);

  // Whether its popover attribute makes it a popover, of which type.
  bool IsPopover() const { return popover_data_; }
  PopoverValueType PopoverType() const;
  // Whether it shows as a popover, which :popover-open matches, as Blink's
  // popoverOpen() says.
  bool PopoverOpen() const;
  PopoverData* GetPopoverData() const { return popover_data_.Get(); }

  enum class HidePopoverTransitionBehavior {
    kFireEventsAndWaitForTransitions,
    kNoEventsNoWaiting,
  };
  void ShowPopoverInternal(Element* invoker,
                           bindings::ExceptionState* exception_state);
  void HidePopoverInternal(Element* invoker,
                           HidePopoverTransitionBehavior transition_behavior,
                           bindings::ExceptionState* exception_state);
  // Hides every open popover above `endpoint` in its stack, or every one for
  // null, as Blink's HideAllPopoversUntil() does.
  static void HideAllPopoversUntil(
      const HTMLElement* endpoint,
      Document& document,
      HidePopoverTransitionBehavior transition_behavior);
  // The open popover that an element entering the top layer, such as a
  // dialog that shows, is nested in, whose stack keeps its popovers open.
  static const HTMLElement* TopLayerElementPopoverAncestor(
      Element& top_layer_element);

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

  // The form element that it belongs to, whose scope an event handler that a
  // content attribute set runs in, as Blink's HTMLElement::formOwner() gives
  // it: none but for a form-associated element.
  virtual Element* FormOwner() const;

  // Element's: an event handler content attribute, such as onclick, sets the
  // handler, as Blink's HTMLElement::ParseAttribute() does, and the popover
  // attribute makes it a popover or not.
  void AttributeChanged(const QualifiedName& name,
                        const std::u16string* old_value,
                        const std::u16string* new_value) override;

  // Node's: the nonce hides as the element goes into a tree, and a popover
  // that leaves the document hides, with no event.
  InsertionNotificationRequest InsertedInto(Node& insertion_point) override;
  void RemovedFrom(Node& insertion_point) override;

  void Trace(cppgc::Visitor* visitor) const override;

 protected:
  // Element's: the copy's event handler content attributes set handlers, as
  // Blink's CloneAttributesFrom() runs them through AttributeChanged().
  void DidCloneAttributes() override;

  // The window's event whose handler the content attribute `name` sets in
  // place of the element's, as a body's and a frameset's do, or empty.
  virtual std::u16string_view WindowEventNameForAttributeName(
      const QualifiedName& name) const;

 private:
  // The handler that the event handler content attribute `name` sets, with
  // `value`, null for none.
  void ParseEventHandlerAttribute(const QualifiedName& name,
                                  const std::u16string* value);
  // Whether it is a summary that is the first summary child of a details
  // element, as Blink's IsMainSummary() says.
  bool IsMainSummary() const;

  enum class PopoverTriggerAction { kShow, kHide, kToggle };
  // Blink's IsPopoverReady(): whether it can be shown or hidden now, or else
  // an exception for one of the reasons that throw.
  bool IsPopoverReady(PopoverTriggerAction action,
                      bindings::ExceptionState* exception_state,
                      bool include_event_handler_text,
                      Document* expected_document) const;
  // Blink's UpdatePopoverAttribute(): a change of type hides it, and no valid
  // value leaves it no popover.
  void UpdatePopoverAttribute(const std::u16string* value);

  cppgc::Member<PopoverData> popover_data_;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_ELEMENT_H_
