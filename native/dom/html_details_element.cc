#include "mitto/dom/html_details_element.h"

#include <utility>

#include "base/functional/bind.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_details_element.h"
#include "mitto/dom/document.h"
#include "mitto/dom/toggle_event.h"
#include "mitto/dom/window.h"
#include "mitto/dom/window_tasks.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

namespace {

// The tasks that fire the event: the window's of the document, or for a
// document of no window, such as one that createHTMLDocument() made, the
// current window's, as such documents of Blink share the window's execution
// context.
WindowTasks* TasksFor(const Document& document) {
  if (Window* const window = document.DefaultView()) {
    return window->GetTasks();
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (!isolate || !isolate->InContext()) {
    return nullptr;
  }
  return Window::Current().GetTasks();
}

}  // namespace

// static
HTMLDetailsElement* HTMLDetailsElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLDetailsElement>(document);
}

HTMLDetailsElement::HTMLDetailsElement(Document& document)
    : HTMLElement(document, u"details") {}

HTMLDetailsElement::~HTMLDetailsElement() = default;

const bindings::WrapperTypeInfo* HTMLDetailsElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLDetailsElement::kWrapperTypeInfo;
}

void HTMLDetailsElement::Trace(cppgc::Visitor* visitor) const {
  HTMLElement::Trace(visitor);
  visitor->Trace(pending_toggle_event_);
}

bool HTMLDetailsElement::IsOpen() const {
  return FindAttribute(u"open");
}

void HTMLDetailsElement::ToggleOpen() {
  if (IsOpen()) {
    RemoveAttribute(QualifiedName{.local_name = u"open"});
  } else {
    SetAttributeWithoutValidation(u"open", std::u16string());
  }
}

Element* HTMLDetailsElement::MainSummary() const {
  for (Element* child = FirstElementChild(); child;
       child = child->NextElementSibling()) {
    if (child->HasHTMLTagName(u"summary")) {
      return child;
    }
  }
  return nullptr;
}

Node::InsertionNotificationRequest HTMLDetailsElement::InsertedInto(
    Node& insertion_point) {
  const InsertionNotificationRequest request =
      HTMLElement::InsertedInto(insertion_point);
  MaybeCloseForExclusivity();
  return request;
}

void HTMLDetailsElement::AttributeChanged(const QualifiedName& name,
                                          const std::u16string* old_value,
                                          const std::u16string* new_value) {
  HTMLElement::AttributeChanged(name, old_value, new_value);
  if (name.namespace_uri) {
    return;
  }
  if (name.local_name == u"open") {
    const bool is_open = new_value;
    if (is_open == static_cast<bool>(old_value)) {
      return;
    }
    QueueToggleEvent(is_open);
    // Opening it closes the others of its name. Blink does so for a change
    // that is not the parser's or a copy's, whose element is in no tree with
    // others yet.
    if (is_open) {
      for (HTMLDetailsElement* const other : OtherElementsInNameGroup()) {
        other->RemoveAttribute(QualifiedName{.local_name = u"open"});
      }
    }
  } else if (name.local_name == u"name") {
    MaybeCloseForExclusivity();
  }
}

Node* HTMLDetailsElement::CloneWithoutChildren(Document& document) const {
  Node* const copy = HTMLElement::CloneWithoutChildren(document);
  // The copy has the same name in the same namespace, so it is a details
  // element too.
  auto* const details = static_cast<HTMLDetailsElement*>(copy);
  if (details->IsOpen()) {
    details->QueueToggleEvent(true);
  }
  return copy;
}

void HTMLDetailsElement::QueueToggleEvent(bool is_open) {
  const std::u16string new_state = is_open ? u"open" : u"closed";
  std::u16string old_state = is_open ? u"closed" : u"open";
  if (pending_toggle_event_) {
    old_state = pending_toggle_event_->OldState();
  }
  pending_toggle_event_ =
      ToggleEvent::CreateTrusted(u"toggle", old_state, new_state);
  const uint64_t task = ++toggle_task_;
  WindowTasks* const tasks = TasksFor(GetDocument());
  if (!tasks) {
    return;
  }
  tasks->PostTask(TaskType::kDOMManipulation,
                  base::BindOnce(
                      [](const cppgc::Persistent<HTMLDetailsElement>& details,
                         uint64_t task) {
                        if (details->toggle_task_ == task) {
                          details->DispatchPendingEvent();
                        }
                      },
                      cppgc::Persistent<HTMLDetailsElement>(this), task));
}

void HTMLDetailsElement::DispatchPendingEvent() {
  const cppgc::Persistent<ToggleEvent> event(pending_toggle_event_.Get());
  pending_toggle_event_ = nullptr;
  DispatchEvent(event.Get());
}

void HTMLDetailsElement::MaybeCloseForExclusivity() {
  if (Name().empty() || !IsOpen()) {
    return;
  }
  for (HTMLDetailsElement* const other : OtherElementsInNameGroup()) {
    if (other->IsOpen()) {
      ToggleOpen();
      return;
    }
  }
}

std::vector<HTMLDetailsElement*>
HTMLDetailsElement::OtherElementsInNameGroup() {
  std::vector<HTMLDetailsElement*> group;
  const std::u16string name = Name();
  if (name.empty()) {
    return group;
  }
  Node& root = TreeRoot();
  for (Node* node = root.FirstChild(); node; node = node->NextInTree(&root)) {
    if (node == this || !node->IsElementNode() ||
        !static_cast<Element*>(node)->HasHTMLTagName(u"details")) {
      continue;
    }
    auto* const details = static_cast<HTMLDetailsElement*>(node);
    if (details->Name() == name) {
      group.push_back(details);
    }
  }
  return group;
}

std::u16string HTMLDetailsElement::Name() const {
  const std::u16string* const name = FindAttribute(u"name");
  return name ? *name : std::u16string();
}

}  // namespace mitto
