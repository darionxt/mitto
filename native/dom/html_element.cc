#include "mitto/dom/html_element.h"

#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "base/functional/bind.h"
#include "base/memory/raw_ref.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_html_element.h"
#include "mitto/dom/document.h"
#include "mitto/dom/event.h"
#include "mitto/dom/event_handler_attributes.h"
#include "mitto/dom/event_target.h"
#include "mitto/dom/html_details_element.h"
#include "mitto/dom/html_dialog_element.h"
#include "mitto/dom/html_input_element.h"
#include "mitto/dom/html_names.h"
#include "mitto/dom/toggle_event.h"
#include "mitto/dom/window_tasks.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

// static
HTMLElement* HTMLElement::Create(Document& document,
                                 const std::u16string& local_name) {
  return bindings::MakeGarbageCollected<HTMLElement>(document, local_name);
}

HTMLElement::HTMLElement(Document& document, const std::u16string& local_name)
    : Element(document, html_names::kXhtmlNamespace, std::nullopt, local_name) {
}

HTMLElement::~HTMLElement() = default;

Element* HTMLElement::FormOwner() const {
  return nullptr;
}

std::u16string_view HTMLElement::WindowEventNameForAttributeName(
    const QualifiedName& name) const {
  return {};
}

void HTMLElement::AttributeChanged(const QualifiedName& name,
                                   const std::u16string* old_value,
                                   const std::u16string* new_value) {
  Element::AttributeChanged(name, old_value, new_value);
  ParseEventHandlerAttribute(name, new_value);
  if (!name.namespace_uri && name.local_name == u"popover") {
    UpdatePopoverAttribute(new_value);
  }
}

void HTMLElement::DidCloneAttributes() {
  for (const Attribute& attribute : attributes()) {
    ParseEventHandlerAttribute(attribute.name, &attribute.value);
    if (!attribute.name.namespace_uri &&
        attribute.name.local_name == u"popover") {
      UpdatePopoverAttribute(&attribute.value);
    }
  }
}

void HTMLElement::Trace(cppgc::Visitor* visitor) const {
  Element::Trace(visitor);
  visitor->Trace(popover_data_);
}

void HTMLElement::ParseEventHandlerAttribute(const QualifiedName& name,
                                             const std::u16string* value) {
  // Blink's HTMLBodyElement's and HTMLFrameSetElement's ParseAttribute() set
  // the window's handlers, before HTMLElement::ParseAttribute() sets the
  // element's.
  if (const std::u16string_view event = WindowEventNameForAttributeName(name);
      !event.empty()) {
    SetWindowAttributeEventListenerFromScriptBody(*this, event, name, value);
    return;
  }
  if (const std::u16string_view event = EventNameForAttributeName(name);
      !event.empty()) {
    SetElementAttributeEventListenerFromScriptBody(*this, event, name, value);
  }
}

const bindings::WrapperTypeInfo* HTMLElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLElement::kWrapperTypeInfo;
}

std::u16string HTMLElement::Autocapitalize() const {
  const std::u16string value = ReflectedStringAttribute(u"autocapitalize");
  if (value.empty()) {
    return std::u16string();
  }
  if (base::EqualsCaseInsensitiveASCII(value, u"none") ||
      base::EqualsCaseInsensitiveASCII(value, u"off")) {
    return u"none";
  }
  if (base::EqualsCaseInsensitiveASCII(value, u"characters")) {
    return u"characters";
  }
  if (base::EqualsCaseInsensitiveASCII(value, u"words")) {
    return u"words";
  }
  // "sentences", "on", or any other value.
  return u"sentences";
}

void HTMLElement::SetAutocapitalize(const std::u16string& value) {
  SetReflectedStringAttribute(u"autocapitalize", value);
}

namespace {

// Blink's contentEditableNormalized(): "true", "false", "plaintext-only",
// or "inherit" for a missing or another value.
std::u16string_view ContentEditableState(const Element& element) {
  const std::u16string* value = element.FindAttribute(u"contenteditable");
  if (!value) {
    return u"inherit";
  }
  if (value->empty() || base::EqualsCaseInsensitiveASCII(*value, u"true")) {
    return u"true";
  }
  if (base::EqualsCaseInsensitiveASCII(*value, u"false")) {
    return u"false";
  }
  if (base::EqualsCaseInsensitiveASCII(*value, u"plaintext-only")) {
    return u"plaintext-only";
  }
  return u"inherit";
}

// Blink's IsEditableOrEditingHost(): an HTML element whose contenteditable is
// true or plaintext-only, or one in a connected document in design mode, is an
// editing host; otherwise a node is editable when its parent is, unless it is
// an HTML element whose contenteditable is false, or an element that is none
// of an HTML element, an SVG svg element and a MathML math element.
bool IsEditableOrEditingHost(const Node& node) {
  const auto* element =
      node.IsElementNode() ? static_cast<const Element*>(&node) : nullptr;
  const bool is_html = element && element->IsHTMLElement();
  if (is_html) {
    const std::u16string_view state = ContentEditableState(*element);
    if (state == u"true" || state == u"plaintext-only") {
      return true;
    }
    if (element->IsConnected() &&
        element->GetDocument().DesignMode() == u"on") {
      return true;
    }
    if (state == u"false") {
      return false;
    }
  }
  const Node* parent = node.ParentNode();
  if (!parent || !IsEditableOrEditingHost(*parent)) {
    return false;
  }
  if (is_html) {
    return true;
  }
  if (element) {
    const QualifiedName& name = element->TagQName();
    return (name.namespace_uri == html_names::kSvgNamespace &&
            name.local_name == u"svg") ||
           (name.namespace_uri == html_names::kMathMlNamespace &&
            name.local_name == u"math");
  }
  return parent->IsElementNode() &&
         static_cast<const Element*>(parent)->IsHTMLElement();
}

// Blink's Element::IsClickableControl(): whether a click at `node` is one
// on a control of its own, a form control, or a node between it and `summary`
// with activation behaviour, an embed or a label.
bool IsClickableControl(const Element& summary, Node* node) {
  if (!node || !node->IsElementNode()) {
    return false;
  }
  for (const std::u16string_view name :
       {u"button", u"fieldset", u"input", u"output", u"select", u"textarea"}) {
    if (static_cast<Element*>(node)->HasHTMLTagName(name)) {
      return true;
    }
  }
  for (; node && node != &summary; node = node->ParentNode()) {
    if (node->HasActivationBehavior()) {
      return true;
    }
    if (node->IsElementNode() &&
        (static_cast<Element*>(node)->HasHTMLTagName(u"embed") ||
         static_cast<Element*>(node)->HasHTMLTagName(u"label"))) {
      return true;
    }
  }
  return false;
}

}  // namespace

int32_t HTMLElement::DefaultTabIndex() const {
  return IsMainSummary() ? 0 : -1;
}

bool HTMLElement::HasActivationBehavior() const {
  return HasHTMLTagName(u"summary");
}

void HTMLElement::RunActivationBehavior(Event& event,
                                        EventDispatchHandlingState* state) {
  if (event.DefaultPrevented() || event.DefaultHandled() || !IsMainSummary()) {
    return;
  }
  EventTarget* const target = event.Target();
  if (IsClickableControl(*this, target ? target->ToNode() : nullptr)) {
    return;
  }
  static_cast<HTMLDetailsElement*>(ParentElement())->ToggleOpen();
  event.SetDefaultHandled();
}

bool HTMLElement::IsMainSummary() const {
  if (!HasHTMLTagName(u"summary")) {
    return false;
  }
  Element* const parent = ParentElement();
  return parent && parent->HasHTMLTagName(u"details") &&
         static_cast<HTMLDetailsElement*>(parent)->MainSummary() == this;
}

v8::Local<v8::Value> HTMLElement::Hidden() const {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const std::u16string* const value = FindAttribute(u"hidden");
  if (!value) {
    return v8::False(isolate);
  }
  if (base::EqualsCaseInsensitiveASCII(*value, u"until-found")) {
    return bindings::ToV8String(isolate, u"until-found");
  }
  return v8::True(isolate);
}

void HTMLElement::SetHidden(
    const std::optional<bindings::BooleanOrUnrestrictedDoubleOrDOMString>&
        value) {
  bool present = false;
  std::u16string attribute_value;
  if (!value) {
    present = false;
  } else if (const bool* const boolean = std::get_if<bool>(&*value)) {
    present = *boolean;
  } else if (const double* const number = std::get_if<double>(&*value)) {
    present = *number != 0 && !std::isnan(*number);
  } else {
    const std::u16string& string = std::get<std::u16string>(*value);
    present = !string.empty();
    if (base::EqualsCaseInsensitiveASCII(string, u"until-found")) {
      attribute_value = u"until-found";
    }
  }
  if (present) {
    SetAttributeWithoutValidation(u"hidden", attribute_value);
  } else {
    RemoveAttribute(QualifiedName{.local_name = u"hidden"});
  }
}

std::u16string HTMLElement::Dir() const {
  const std::u16string* value = FindAttribute(u"dir");
  if (value) {
    for (const std::u16string_view keyword : {u"ltr", u"rtl", u"auto"}) {
      if (base::EqualsCaseInsensitiveASCII(*value, keyword)) {
        return std::u16string(keyword);
      }
    }
  }
  return std::u16string();
}

void HTMLElement::SetDir(const std::u16string& value) {
  SetAttributeWithoutValidation(u"dir", value);
}

bool HTMLElement::Translate() const {
  for (const Element* element = this; element;
       element = element->ParentElement()) {
    if (!element->IsHTMLElement()) {
      continue;
    }
    const std::u16string* value = element->FindAttribute(u"translate");
    if (!value) {
      continue;
    }
    if (value->empty() || base::EqualsCaseInsensitiveASCII(*value, u"yes")) {
      return true;
    }
    if (base::EqualsCaseInsensitiveASCII(*value, u"no")) {
      return false;
    }
  }
  return true;
}

void HTMLElement::SetTranslate(bool value) {
  SetAttributeWithoutValidation(u"translate", value ? u"yes" : u"no");
}

bool HTMLElement::Draggable() const {
  const std::u16string* value = FindAttribute(u"draggable");
  return value && base::EqualsCaseInsensitiveASCII(*value, u"true");
}

void HTMLElement::SetDraggable(bool value) {
  SetAttributeWithoutValidation(u"draggable", value ? u"true" : u"false");
}

bool HTMLElement::Spellcheck() const {
  // Blink's IsSpellCheckingEnabled(), with Chrome's setting on by default.
  for (const Element* element = this; element;
       element = element->ParentElement()) {
    const std::u16string* value = element->FindAttribute(u"spellcheck");
    if (value &&
        (value->empty() || base::EqualsCaseInsensitiveASCII(*value, u"true"))) {
      return true;
    }
    if (value && base::EqualsCaseInsensitiveASCII(*value, u"false")) {
      return false;
    }
    // The default state: off in an input that has been a password field.
    if (element->HasHTMLTagName(u"input") &&
        static_cast<const HTMLInputElement*>(element)->HasBeenPasswordField()) {
      return false;
    }
  }
  return true;
}

void HTMLElement::SetSpellcheck(bool value) {
  SetAttributeWithoutValidation(u"spellcheck", value ? u"true" : u"false");
}

std::u16string HTMLElement::WritingSuggestions() const {
  for (const Element* element = this; element;
       element = element->ParentElement()) {
    const std::u16string* const value =
        element->FindAttribute(u"writingsuggestions");
    if (!value) {
      continue;
    }
    // The invalid value default is "true".
    return base::EqualsCaseInsensitiveASCII(*value, u"false") ? u"false"
                                                              : u"true";
  }
  return u"true";
}

void HTMLElement::SetWritingSuggestions(const std::u16string& value) {
  SetAttributeWithoutValidation(u"writingsuggestions", value);
}

std::u16string HTMLElement::ContentEditable() const {
  return std::u16string(ContentEditableState(*this));
}

void HTMLElement::SetContentEditable(
    const std::u16string& value,
    bindings::ExceptionState& exception_state) {
  const std::u16string lower = base::ToLowerASCII(value);
  if (lower == u"true" || lower == u"false" || lower == u"plaintext-only") {
    SetAttributeWithoutValidation(u"contenteditable", lower);
  } else if (lower == u"inherit") {
    RemoveAttribute(QualifiedName{.local_name = u"contenteditable"});
  } else {
    exception_state.ThrowDOMException(
        "SyntaxError",
        base::StrCat({"The value provided ('", base::UTF16ToUTF8(value),
                      "') is not one of 'true', 'false', 'plaintext-only', "
                      "or 'inherit'."}));
  }
}

bool HTMLElement::IsContentEditableForBinding() const {
  return IsEditableOrEditingHost(*this);
}

// The Popover API.

namespace {

using PopoverStack = HeapVector<HTMLElement>;

bool StackContains(const PopoverStack& stack, const HTMLElement* element) {
  for (size_t i = 0; i < stack.size(); ++i) {
    if (stack[i] == element) {
      return true;
    }
  }
  return false;
}

// The position of `element` in the stack, as Blink's Find(), or none.
std::optional<size_t> StackFind(const PopoverStack& stack,
                                const HTMLElement* element) {
  for (size_t i = 0; i < stack.size(); ++i) {
    if (stack[i] == element) {
      return i;
    }
  }
  return std::nullopt;
}

void StackErase(PopoverStack& stack, const HTMLElement* element) {
  stack.EraseIf([element](HTMLElement* item) { return item == element; });
}

PopoverValueType PopoverTypeFromAttributeValue(const std::u16string* value) {
  if (!value) {
    return PopoverValueType::kNone;
  }
  if (value->empty() || base::EqualsCaseInsensitiveASCII(*value, u"auto")) {
    return PopoverValueType::kAuto;
  }
  if (base::EqualsCaseInsensitiveASCII(*value, u"hint")) {
    return PopoverValueType::kHint;
  }
  // Invalid values default to popover=manual.
  return PopoverValueType::kManual;
}

// The task that a popover's toggle event goes in, which a later one cancels.
uint64_t NextToggleEventTask() {
  static uint64_t last_task = 0;
  return ++last_task;
}

// Queues `event` at `popover` in a task of the DOM manipulation source,
// which `data` cancels by its number if `cancelable`, as Blink's
// PostCancellableTask() handle.
void QueueToggleEvent(HTMLElement& popover,
                      PopoverData* data,
                      ToggleEvent* event,
                      bool cancelable) {
  const uint64_t task = NextToggleEventTask();
  if (cancelable) {
    data->set_pending_toggle_event_task(task);
  }
  WindowTasks* const tasks = popover.GetDocument().GetTasks();
  if (!tasks) {
    return;
  }
  tasks->PostTask(TaskType::kDOMManipulation,
                  base::BindOnce(
                      [](const cppgc::Persistent<HTMLElement>& popover,
                         const cppgc::Persistent<PopoverData>& data,
                         const cppgc::Persistent<ToggleEvent>& event,
                         uint64_t task, bool cancelable) {
                        if (cancelable) {
                          if (data->pending_toggle_event_task() != task) {
                            return;
                          }
                          data->cancel_pending_toggle_event_task();
                        }
                        popover->DispatchEvent(event.Get());
                      },
                      cppgc::Persistent<HTMLElement>(&popover),
                      cppgc::Persistent<PopoverData>(data),
                      cppgc::Persistent<ToggleEvent>(event), task, cancelable));
}

// Blink's ScopedPopoverShowing, with PopoverHintNewBehavior: the document is
// showing a popover meanwhile.
class ScopedPopoverShowing {
 public:
  explicit ScopedPopoverShowing(Document& document)
      : document_(document), was_being_shown_(document.PopoverShowing()) {
    document_->SetPopoverShowing(true);
  }
  ScopedPopoverShowing(const ScopedPopoverShowing&) = delete;
  ScopedPopoverShowing& operator=(const ScopedPopoverShowing&) = delete;
  ~ScopedPopoverShowing() { document_->SetPopoverShowing(was_being_shown_); }

 private:
  const raw_ref<Document> document_;
  const bool was_being_shown_;
};

// Blink's ScopedPopoverHiding: the document is hiding one more popover, and
// the popover is being hidden, meanwhile.
class ScopedPopoverHiding {
 public:
  explicit ScopedPopoverHiding(HTMLElement& popover)
      : popover_(popover),
        document_(popover.GetDocument()),
        was_hiding_(popover.GetPopoverData()->hiding_this_popover()) {
    document_->IncrementPopoverHidingNestingCount();
    if (!was_hiding_) {
      popover_->GetPopoverData()->set_hiding_this_popover(true);
    }
  }
  ScopedPopoverHiding(const ScopedPopoverHiding&) = delete;
  ScopedPopoverHiding& operator=(const ScopedPopoverHiding&) = delete;
  ~ScopedPopoverHiding() {
    document_->DecrementPopoverHidingNestingCount();
    if (!was_hiding_ && popover_->GetPopoverData()) {
      popover_->GetPopoverData()->set_hiding_this_popover(false);
    }
  }
  bool WasHiding() const { return was_hiding_; }

 private:
  const raw_ref<HTMLElement> popover_;
  const raw_ref<Document> document_;
  const bool was_hiding_;
};

// NearestOpenPopover(): `node` or its nearest ancestor that is an open
// popover of the auto or hint type.
const HTMLElement* NearestOpenPopover(const Node* node) {
  for (; node; node = node->ParentNode()) {
    if (!node->IsElementNode() ||
        !static_cast<const Element*>(node)->IsHTMLElement()) {
      continue;
    }
    const auto* candidate = static_cast<const HTMLElement*>(node);
    if (candidate->PopoverOpen() &&
        candidate->PopoverType() != PopoverValueType::kManual) {
      return candidate;
    }
  }
  return nullptr;
}

// Blink's FindTopmostPopoverAncestor() with PopoverHintNewBehavior: of the
// open popovers that the element's parent and the invoker are in, the one
// highest in the auto stack then the hint stack.
HTMLElement* FindTopmostPopoverAncestor(
    Element& new_popover_or_top_layer_element,
    Element* new_popovers_invoker,
    bool is_popover) {
  Document& document = new_popover_or_top_layer_element.GetDocument();
  std::vector<const HTMLElement*> positions;
  for (size_t i = 0; i < document.PopoverAutoStack().size(); ++i) {
    positions.push_back(document.PopoverAutoStack()[i]);
  }
  for (size_t i = 0; i < document.PopoverHintStack().size(); ++i) {
    positions.push_back(document.PopoverHintStack()[i]);
  }
  if (is_popover) {
    positions.push_back(
        static_cast<HTMLElement*>(&new_popover_or_top_layer_element));
  }
  const auto position_of =
      [&positions](const HTMLElement* popover) -> std::optional<size_t> {
    // The last, as Blink's map keeps the last position it set.
    for (size_t i = positions.size(); i > 0; --i) {
      if (positions[i - 1] == popover) {
        return i - 1;
      }
    }
    return std::nullopt;
  };
  const HTMLElement* topmost = nullptr;
  const auto check_ancestor = [&](const Element* to_check) {
    const HTMLElement* const candidate = NearestOpenPopover(to_check);
    if (!candidate) {
      return;
    }
    const std::optional<size_t> position = position_of(candidate);
    if (!position) {
      return;
    }
    if (!topmost || *position_of(topmost) < *position) {
      topmost = candidate;
    }
  };
  check_ancestor(new_popover_or_top_layer_element.ParentElement());
  check_ancestor(new_popovers_invoker);
  return const_cast<HTMLElement*>(topmost);
}

// CloseEntirePopoverStack(): hides the stack's popovers, the topmost first.
void CloseEntirePopoverStack(
    PopoverStack& stack,
    HTMLElement::HidePopoverTransitionBehavior transition_behavior) {
  while (!stack.empty()) {
    HTMLElement* const stack_top = stack.back();
    stack_top->HidePopoverInternal(/*invoker=*/nullptr, transition_behavior,
                                   /*exception_state=*/nullptr);
    // One that could not hide stays: stop rather than loop.
    if (!stack.empty() && stack.back() == stack_top) {
      return;
    }
  }
}

}  // namespace

PopoverValueType HTMLElement::PopoverType() const {
  return popover_data_ ? popover_data_->type() : PopoverValueType::kNone;
}

bool HTMLElement::PopoverOpen() const {
  return popover_data_ &&
         popover_data_->visibility_state() == PopoverVisibilityState::kShowing;
}

void HTMLElement::UpdatePopoverAttribute(const std::u16string* value) {
  PopoverValueType type = PopoverTypeFromAttributeValue(value);
  if (IsPopover()) {
    if (PopoverType() == type) {
      return;
    }
    // If the popover type is changing, hide it.
    if (PopoverOpen()) {
      HidePopoverInternal(
          /*invoker=*/nullptr,
          HidePopoverTransitionBehavior::kFireEventsAndWaitForTransitions,
          /*exception_state=*/nullptr);
      // Event handlers could have changed the popover attribute.
      type = PopoverTypeFromAttributeValue(FindAttribute(u"popover"));
    }
  }
  if (type == PopoverValueType::kNone) {
    // If the popover attribute is being removed, remove the PopoverData.
    popover_data_ = nullptr;
    return;
  }
  if (popover_data_) {
    popover_data_->set_type(type);
  } else {
    popover_data_ = bindings::MakeGarbageCollected<PopoverData>(type);
  }
}

bool HTMLElement::IsPopoverReady(PopoverTriggerAction action,
                                 bindings::ExceptionState* exception_state,
                                 bool include_event_handler_text,
                                 Document* expected_document) const {
  const auto maybe_throw = [&](const char* name, std::string_view message) {
    if (exception_state) {
      exception_state->ThrowDOMException(
          name, base::StrCat({message, include_event_handler_text
                                           ? " This might have been the result "
                                             "of the \"beforetoggle\" event "
                                             "handler changing the state of "
                                             "this popover."
                                           : ""}));
    }
  };
  if (!IsPopover()) {
    maybe_throw("NotSupportedError",
                "Not supported on elements that are not popovers.");
    return false;
  }
  if (!GetDocument().DefaultView()) {
    maybe_throw(
        "InvalidStateError",
        "Invalid for popovers within documents that are not fully active.");
    return false;
  }
  if (action == PopoverTriggerAction::kShow &&
      popover_data_->visibility_state() != PopoverVisibilityState::kHidden) {
    return false;
  }
  if (action == PopoverTriggerAction::kHide &&
      popover_data_->visibility_state() != PopoverVisibilityState::kShowing) {
    return false;
  }
  // A subtree that is leaving the document counts as connected while its
  // popovers hide, as Blink's nodes do until their own RemovedFrom().
  Node* const removal_root = GetDocument().PopoverRemovalRoot();
  if (!IsConnected() && !(removal_root && &TreeRoot() == removal_root)) {
    maybe_throw("InvalidStateError",
                "Invalid on disconnected popover elements.");
    return false;
  }
  if (expected_document && &GetDocument() != expected_document) {
    maybe_throw("InvalidStateError",
                "Invalid when the document changes while showing or hiding a "
                "popover element.");
    return false;
  }
  if (action == PopoverTriggerAction::kShow && HasHTMLTagName(u"dialog") &&
      static_cast<const HTMLDialogElement*>(this)->IsModal()) {
    maybe_throw("InvalidStateError",
                "The dialog is already open as a dialog, and therefore cannot "
                "be opened as a popover.");
    return false;
  }
  // PopoverHintNewBehavior and PopoverHintNestedShowException: a popover
  // cannot show while another shows or hides, but for the check again after
  // its own beforetoggle.
  if (!expected_document && action == PopoverTriggerAction::kShow &&
      (GetDocument().PopoverShowing() ||
       GetDocument().PopoverHidingNestingCount())) {
    maybe_throw("InvalidStateError",
                "Invalid to show a popover during another show operation");
    return false;
  }
  return true;
}

bool HTMLElement::TogglePopover(
    const std::optional<bindings::TogglePopoverOptionsOrBoolean>& options,
    bindings::ExceptionState& exception_state) {
  const bool popover_was_open = PopoverOpen();
  bool force = !popover_was_open;
  Element* invoker = nullptr;
  if (options) {
    if (const bool* value = std::get_if<bool>(&*options)) {
      force = *value;
    } else {
      const auto& dictionary =
          std::get<bindings::TogglePopoverOptions>(*options);
      if (dictionary.force) {
        force = *dictionary.force;
      }
      invoker = dictionary.source.value_or(nullptr);
    }
  }
  if (!force && popover_was_open) {
    HidePopover(exception_state);
  } else if (force && !popover_was_open) {
    ShowPopoverInternal(invoker, &exception_state);
  } else {
    // The state already lines up; the other exceptions still throw.
    IsPopoverReady(PopoverTriggerAction::kToggle, &exception_state,
                   /*include_event_handler_text=*/false,
                   /*expected_document=*/nullptr);
  }
  return PopoverOpen();
}

void HTMLElement::ShowPopover(
    const std::optional<bindings::ShowPopoverOptions>& options,
    bindings::ExceptionState& exception_state) {
  ShowPopoverInternal(options ? options->source.value_or(nullptr) : nullptr,
                      &exception_state);
}

void HTMLElement::HidePopover(bindings::ExceptionState& exception_state) {
  HidePopoverInternal(
      /*invoker=*/nullptr,
      HidePopoverTransitionBehavior::kFireEventsAndWaitForTransitions,
      &exception_state);
}

void HTMLElement::ShowPopoverInternal(
    Element* invoker,
    bindings::ExceptionState* exception_state) {
  if (!IsPopoverReady(PopoverTriggerAction::kShow, exception_state,
                      /*include_event_handler_text=*/false,
                      /*expected_document=*/nullptr)) {
    return;
  }
  ScopedPopoverShowing scoped_showing(GetDocument());
  const cppgc::Persistent<HTMLElement> self(this);
  Document& original_document = GetDocument();

  // Fire the "opening" beforetoggle event.
  ToggleEvent* const event =
      ToggleEvent::CreateTrusted(u"beforetoggle", u"closed", u"open",
                                 /*cancelable=*/true, invoker);
  // ShadowRootReferenceTarget makes an invoker's events composed.
  event->SetComposed(invoker != nullptr);
  if (!DispatchEvent(event)) {
    return;
  }
  // The beforetoggle event handler could have changed this popover.
  if (!IsPopoverReady(PopoverTriggerAction::kShow, exception_state,
                      /*include_event_handler_text=*/true,
                      &original_document)) {
    return;
  }

  const PopoverValueType original_type = PopoverType();
  bool new_popover_is_auto = original_type == PopoverValueType::kAuto;
  if (new_popover_is_auto || original_type == PopoverValueType::kHint) {
    PopoverStack& auto_stack = original_document.PopoverAutoStack();
    PopoverStack& hint_stack = original_document.PopoverHintStack();
    PopoverStack* append_to_stack = nullptr;
    const auto check_popover_validity = [&](const char* message) {
      if (PopoverType() != original_type) {
        if (exception_state) {
          exception_state->ThrowDOMException("InvalidStateError", message);
        }
        return false;
      }
      return IsPopoverReady(PopoverTriggerAction::kShow, exception_state,
                            /*include_event_handler_text=*/true,
                            &original_document);
    };
    constexpr auto kTransition =
        HidePopoverTransitionBehavior::kFireEventsAndWaitForTransitions;
    HTMLElement* parent = FindTopmostPopoverAncestor(*this, invoker,
                                                     /*is_popover=*/true);
    if (parent && StackContains(hint_stack, parent)) {
      // An auto popover nested in a hint is demoted to a hint.
      new_popover_is_auto = false;
      HideAllPopoversUntil(parent, original_document, kTransition);
      append_to_stack = &hint_stack;
    } else {
      CloseEntirePopoverStack(hint_stack, kTransition);
      original_document.SetPopoverHintStackParent(nullptr);
      if (!check_popover_validity("The value of the popover attribute was "
                                  "changed while hiding hint popovers.")) {
        return;
      }
      parent = FindTopmostPopoverAncestor(*this, invoker, /*is_popover=*/true);
      if (new_popover_is_auto) {
        HideAllPopoversUntil(parent, original_document, kTransition);
        if (!check_popover_validity(
                "The popover attribute changed while hiding other "
                "popovers.")) {
          return;
        }
        append_to_stack = &auto_stack;
      } else {
        if (parent && hint_stack.empty()) {
          original_document.SetPopoverHintStackParent(parent);
        }
        append_to_stack = &hint_stack;
      }
    }
    // The beforetoggle event handlers could have changed this popover.
    if (!check_popover_validity("The value of the popover attribute was "
                                "changed while hiding popovers.")) {
      return;
    }
    // Add this popover to the appropriate popover stack.
    append_to_stack->push_back(this);
  }

  popover_data_->set_visibility_state(PopoverVisibilityState::kShowing);
  popover_data_->set_invoker(invoker);

  // Queue the "opening" toggle event, keeping the old state of one that is
  // queued already.
  std::u16string old_state = u"closed";
  if (popover_data_->has_pending_toggle_event_task()) {
    old_state = popover_data_->pending_toggle_event_started_closed() ? u"closed"
                                                                     : u"open";
    popover_data_->cancel_pending_toggle_event_task();
  } else {
    popover_data_->set_pending_toggle_event_started_closed(true);
  }
  ToggleEvent* const after_event = ToggleEvent::CreateTrusted(
      u"toggle", old_state, u"open", /*cancelable=*/false, invoker);
  after_event->SetComposed(invoker != nullptr);
  QueueToggleEvent(*this, popover_data_, after_event, /*cancelable=*/true);
}

void HTMLElement::HidePopoverInternal(
    Element* invoker,
    HidePopoverTransitionBehavior transition_behavior,
    bindings::ExceptionState* exception_state) {
  if (!IsPopoverReady(PopoverTriggerAction::kHide, exception_state,
                      /*include_event_handler_text=*/true,
                      /*expected_document=*/nullptr)) {
    return;
  }
  const cppgc::Persistent<HTMLElement> self(this);
  Document& document = GetDocument();
  ScopedPopoverHiding scoped_hiding(*this);
  if (scoped_hiding.WasHiding()) {
    transition_behavior = HidePopoverTransitionBehavior::kNoEventsNoWaiting;
  }

  PopoverStack& hint_stack = document.PopoverHintStack();
  PopoverStack& auto_stack = document.PopoverAutoStack();
  PopoverStack* stack_containing_this = nullptr;
  if (PopoverType() == PopoverValueType::kAuto ||
      PopoverType() == PopoverValueType::kHint) {
    // Hide any popovers above us in the stack.
    if (document.PopoverHintStackParent() == this) {
      CloseEntirePopoverStack(hint_stack, transition_behavior);
      document.SetPopoverHintStackParent(nullptr);
    }
    HideAllPopoversUntil(this, document, transition_behavior);
    // The 'beforetoggle' event handlers could have changed this popover.
    if (!IsPopoverReady(PopoverTriggerAction::kHide, exception_state,
                        /*include_event_handler_text=*/true, &document)) {
      return;
    }
    stack_containing_this = !auto_stack.empty() && auto_stack.back() == this
                                ? &auto_stack
                                : &hint_stack;
  }

  if (transition_behavior ==
      HidePopoverTransitionBehavior::kFireEventsAndWaitForTransitions) {
    // Fire the "closing" beforetoggle event, which cannot be canceled.
    ToggleEvent* const event =
        ToggleEvent::CreateTrusted(u"beforetoggle", u"open", u"closed",
                                   /*cancelable=*/false, invoker);
    event->SetComposed(invoker != nullptr);
    DispatchEvent(event);
    if (!IsPopoverReady(PopoverTriggerAction::kHide, exception_state,
                        /*include_event_handler_text=*/true, &document)) {
      return;
    }
    // A beforetoggle event handler that showed another popover above this
    // one: it hides again, with no events.
    if (stack_containing_this && !stack_containing_this->empty() &&
        stack_containing_this->back() != this) {
      HideAllPopoversUntil(this, document,
                           HidePopoverTransitionBehavior::kNoEventsNoWaiting);
      if (!IsPopoverReady(PopoverTriggerAction::kHide, exception_state,
                          /*include_event_handler_text=*/true, &document)) {
        return;
      }
    }

    // Queue the "closing" toggle event, keeping the old state of one that is
    // queued already.
    std::u16string old_state = u"open";
    if (popover_data_->has_pending_toggle_event_task()) {
      old_state = popover_data_->pending_toggle_event_started_closed()
                      ? u"closed"
                      : u"open";
      popover_data_->cancel_pending_toggle_event_task();
    } else {
      popover_data_->set_pending_toggle_event_started_closed(false);
    }
    ToggleEvent* const after_event = ToggleEvent::CreateTrusted(
        u"toggle", old_state, u"closed", /*cancelable=*/false, invoker);
    after_event->SetComposed(invoker != nullptr);
    QueueToggleEvent(*this, popover_data_, after_event, /*cancelable=*/true);
  }

  if (PopoverType() != PopoverValueType::kManual) {
    if (StackContains(hint_stack, this)) {
      StackErase(hint_stack, this);
      if (hint_stack.empty()) {
        document.SetPopoverHintStackParent(nullptr);
      }
    } else if (StackContains(auto_stack, this)) {
      StackErase(auto_stack, this);
    }
  }
  popover_data_->set_invoker(nullptr);
  popover_data_->set_visibility_state(PopoverVisibilityState::kHidden);
}

// static
void HTMLElement::HideAllPopoversUntil(
    const HTMLElement* endpoint,
    Document& document,
    HidePopoverTransitionBehavior transition_behavior) {
  if (endpoint && !endpoint->PopoverOpen()) {
    return;
  }
  if (!endpoint) {
    CloseEntirePopoverStack(document.PopoverHintStack(), transition_behavior);
    document.SetPopoverHintStackParent(nullptr);
    CloseEntirePopoverStack(document.PopoverAutoStack(), transition_behavior);
    return;
  }

  // Hides the popovers above `until` in `stack`, the topmost first, as
  // Blink's hide_stack_until with PopoverHintNewBehavior.
  const auto hide_stack_until = [transition_behavior](const HTMLElement* until,
                                                      PopoverStack& stack) {
    if (!StackContains(stack, until)) {
      CloseEntirePopoverStack(stack, transition_behavior);
      return;
    }
    // The popover just above `until`, which is the last to hide.
    const std::optional<size_t> position = StackFind(stack, until);
    if (*position + 1 >= stack.size()) {
      return;
    }
    const HTMLElement* const last_to_hide = stack[*position + 1];
    while (last_to_hide && last_to_hide->PopoverOpen() && !stack.empty()) {
      HTMLElement* const stack_top = stack.back();
      stack_top->HidePopoverInternal(/*invoker=*/nullptr, transition_behavior,
                                     /*exception_state=*/nullptr);
      // One that could not hide stays: stop rather than loop.
      if (!stack.empty() && stack.back() == stack_top) {
        return;
      }
    }
  };

  PopoverStack& hint_stack = document.PopoverHintStack();
  PopoverStack& auto_stack = document.PopoverAutoStack();
  if (StackContains(hint_stack, endpoint)) {
    hide_stack_until(endpoint, hint_stack);
    if (document.PopoverHidingNestingCount() == 0) {
      HTMLElement* const hint_parent = document.PopoverHintStackParent();
      if (hint_parent && StackContains(auto_stack, hint_parent)) {
        hide_stack_until(hint_parent, auto_stack);
      } else {
        CloseEntirePopoverStack(auto_stack, transition_behavior);
      }
    }
    return;
  }
  if (!StackContains(auto_stack, endpoint)) {
    return;
  }
  if (document.PopoverHidingNestingCount() == 0) {
    CloseEntirePopoverStack(hint_stack, transition_behavior);
    document.SetPopoverHintStackParent(nullptr);
  }
  if (!StackContains(auto_stack, endpoint)) {
    return;
  }
  hide_stack_until(endpoint, auto_stack);
}

// static
const HTMLElement* HTMLElement::TopLayerElementPopoverAncestor(
    Element& top_layer_element) {
  return FindTopmostPopoverAncestor(top_layer_element,
                                    /*new_popovers_invoker=*/nullptr,
                                    /*is_popover=*/false);
}

Node::InsertionNotificationRequest HTMLElement::InsertedInto(
    Node& insertion_point) {
  const InsertionNotificationRequest request =
      Element::InsertedInto(insertion_point);
  HideNonce();
  return request;
}

void HTMLElement::RemovedFrom(Node& insertion_point) {
  // If a popover is removed from the document, it hides with no events, as
  // Blink does before Element::RemovedFrom().
  if (IsPopover() && insertion_point.IsConnected() &&
      !GetDocument().StatePreservingAtomicMoveInProgress()) {
    Document& document = GetDocument();
    Node* const previous_root = document.PopoverRemovalRoot();
    document.SetPopoverRemovalRoot(&TreeRoot());
    HidePopoverInternal(/*invoker=*/nullptr,
                        HidePopoverTransitionBehavior::kNoEventsNoWaiting,
                        /*exception_state=*/nullptr);
    document.SetPopoverRemovalRoot(previous_root);
  }
  Element::RemovedFrom(insertion_point);
}

}  // namespace mitto
