#include "mitto/dom/html_dialog_element.h"

#include "base/auto_reset.h"
#include "base/functional/bind.h"
#include "base/notreached.h"
#include "base/strings/string_util.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_dialog_element.h"
#include "mitto/dom/document.h"
#include "mitto/dom/event.h"
#include "mitto/dom/toggle_event.h"
#include "mitto/dom/window.h"
#include "mitto/dom/window_tasks.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

// static
HTMLDialogElement* HTMLDialogElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLDialogElement>(document);
}

HTMLDialogElement::HTMLDialogElement(Document& document)
    : HTMLElement(document, u"dialog") {}

HTMLDialogElement::~HTMLDialogElement() = default;

const bindings::WrapperTypeInfo* HTMLDialogElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLDialogElement::kWrapperTypeInfo;
}

void HTMLDialogElement::Trace(cppgc::Visitor* visitor) const {
  HTMLElement::Trace(visitor);
  visitor->Trace(pending_toggle_event_);
}

bool HTMLDialogElement::IsOpen() const {
  return FindAttribute(u"open");
}

bool HTMLDialogElement::IsOpenAndActive() const {
  return IsOpen() && IsConnected() && GetDocument().DefaultView();
}

HTMLDialogElement::ClosedByState HTMLDialogElement::ClosedByStateOf() const {
  if (const std::u16string* const value = FindAttribute(u"closedby")) {
    const std::u16string lower = base::ToLowerASCII(*value);
    if (lower == u"any") {
      return ClosedByState::kAny;
    }
    if (lower == u"none") {
      return ClosedByState::kNone;
    }
    if (lower == u"closerequest") {
      return ClosedByState::kCloseRequest;
    }
  }
  // The missing and invalid value default, Auto: closerequest for a modal
  // dialog, none for another.
  return is_modal_ ? ClosedByState::kCloseRequest : ClosedByState::kNone;
}

std::u16string HTMLDialogElement::ClosedBy() const {
  switch (ClosedByStateOf()) {
    case ClosedByState::kAny:
      return u"any";
    case ClosedByState::kCloseRequest:
      return u"closerequest";
    case ClosedByState::kNone:
      return u"none";
  }
  NOTREACHED();
}

void HTMLDialogElement::SetClosedBy(const std::u16string& value) {
  SetAttributeWithoutValidation(u"closedby", value);
}

void HTMLDialogElement::Show(bindings::ExceptionState& exception_state) {
  if (IsOpen()) {
    if (is_modal_) {
      exception_state.ThrowDOMException(
          "InvalidStateError",
          "The dialog is already open as a modal dialog, and therefore "
          "cannot be opened as a non-modal dialog.");
    }
    return;
  }
  if (!DispatchToggleEvents(/*opening=*/true, /*as_modal=*/false)) {
    return;
  }
  SetAttributeWithoutValidation(u"open", std::u16string());
}

void HTMLDialogElement::ShowModal(bindings::ExceptionState& exception_state) {
  if (IsOpen()) {
    if (!is_modal_) {
      exception_state.ThrowDOMException(
          "InvalidStateError",
          "The dialog is already open as a non-modal dialog, and therefore "
          "cannot be opened as a modal dialog.");
    }
    return;
  }
  if (!IsConnected()) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      "The element is not in a Document.");
    return;
  }
  if (!GetDocument().DefaultView()) {
    exception_state.ThrowDOMException(
        "InvalidStateError",
        "Invalid for dialogs within documents that are not fully active.");
    return;
  }
  if (!DispatchToggleEvents(/*opening=*/true, /*as_modal=*/true)) {
    return;
  }
  is_modal_ = true;
  // This makes the close watcher, now that the dialog is modal.
  SetAttributeWithoutValidation(u"open", std::u16string());
  SetCloseWatcherEnabledState();
}

void HTMLDialogElement::Close(
    const std::optional<std::u16string>& return_value) {
  if (is_closing_) {
    return;
  }
  const base::AutoReset<bool> closing(&is_closing_, true);
  if (!IsOpen()) {
    return;
  }
  DispatchToggleEvents(/*opening=*/false, /*as_modal=*/false);
  // A beforetoggle listener may have removed the open attribute itself.
  if (!IsOpen()) {
    return;
  }
  RemoveAttribute(QualifiedName{.local_name = u"open"});
  is_modal_ = false;
  if (return_value) {
    return_value_ = *return_value;
  }
  ScheduleCloseEvent();
  DestroyCloseWatcher();
}

// RequestCloseInternal() and CloseWatcher::RequestClose(): cancel, which a
// listener may cancel, then close() with the return value, if the watcher
// that fired cancel is still there and enabled.
void HTMLDialogElement::RequestClose(
    const std::optional<std::u16string>& return_value,
    bindings::ExceptionState& exception_state) {
  if (!IsOpenAndActive() || !close_watcher_) {
    return;
  }
  close_watcher_enabled_ = true;
  request_close_return_value_ = return_value;
  const uint64_t watcher = close_watcher_;
  if (close_watcher_dispatching_cancel_ != watcher) {
    bindings::EventInit init;
    init.cancelable = true;
    Event* const cancel = Event::Create(u"cancel", init);
    cancel->SetTrusted(true);
    {
      const base::AutoReset<uint64_t> dispatching(
          &close_watcher_dispatching_cancel_, watcher);
      DispatchEvent(cancel);
    }
    if (!cancel->DefaultPrevented() && close_watcher_ == watcher &&
        close_watcher_enabled_ && GetDocument().DefaultView()) {
      DestroyCloseWatcher();
      Close(request_close_return_value_);
    }
  }
  SetCloseWatcherEnabledState();
}

Node::InsertionNotificationRequest HTMLDialogElement::InsertedInto(
    Node& insertion_point) {
  const InsertionNotificationRequest request =
      HTMLElement::InsertedInto(insertion_point);
  if (IsOpen() && insertion_point.IsConnected()) {
    CreateCloseWatcher();
  }
  return request;
}

void HTMLDialogElement::RemovedFrom(Node& insertion_point) {
  HTMLElement::RemovedFrom(insertion_point);
  if (!insertion_point.IsConnected()) {
    return;
  }
  is_modal_ = false;
  DestroyCloseWatcher();
}

void HTMLDialogElement::AttributeChanged(const QualifiedName& name,
                                         const std::u16string* old_value,
                                         const std::u16string* new_value) {
  HTMLElement::AttributeChanged(name, old_value, new_value);
  if (name.namespace_uri) {
    return;
  }
  if (name.local_name == u"closedby") {
    const bool same = old_value && new_value ? *old_value == *new_value
                                             : old_value == new_value;
    if (!same) {
      SetCloseWatcherEnabledState();
    }
  } else if (name.local_name == u"open" && !is_closing_) {
    // Opening or closing it by the attribute alone fires no event, and a
    // modal one stays modal.
    if (!new_value) {
      DestroyCloseWatcher();
    } else if (!old_value && IsConnected()) {
      CreateCloseWatcher();
    }
  }
}

bool HTMLDialogElement::DispatchToggleEvents(bool opening, bool as_modal) {
  std::u16string old_state = opening ? u"closed" : u"open";
  const std::u16string new_state = opening ? u"open" : u"closed";
  ToggleEvent* const before_event = ToggleEvent::CreateTrusted(
      u"beforetoggle", old_state, new_state, /*cancelable=*/opening);
  if (!DispatchEvent(before_event)) {
    return false;
  }
  if (opening && (IsOpen() || (as_modal && !IsConnected()))) {
    return false;
  }
  if (pending_toggle_event_) {
    old_state = pending_toggle_event_->OldState();
  }
  pending_toggle_event_ =
      ToggleEvent::CreateTrusted(u"toggle", old_state, new_state);
  const uint64_t task = ++toggle_task_;
  WindowTasks* const tasks = GetDocument().GetTasks();
  if (!tasks) {
    return true;
  }
  tasks->PostTask(TaskType::kDOMManipulation,
                  base::BindOnce(
                      [](const cppgc::Persistent<HTMLDialogElement>& dialog,
                         uint64_t task) {
                        if (dialog->toggle_task_ == task) {
                          dialog->DispatchPendingToggleEvent();
                        }
                      },
                      cppgc::Persistent<HTMLDialogElement>(this), task));
  return true;
}

// The pending event goes only once the dispatch is done, as in Blink, so a
// show() or close() in a toggle listener queues no toggle event of its own.
void HTMLDialogElement::DispatchPendingToggleEvent() {
  if (!pending_toggle_event_) {
    return;
  }
  DispatchEvent(pending_toggle_event_.Get());
  pending_toggle_event_ = nullptr;
}

void HTMLDialogElement::ScheduleCloseEvent() {
  Window* const window = GetDocument().DefaultView();
  if (!window) {
    return;
  }
  Event* const event = Event::Create(u"close", bindings::EventInit());
  event->SetTrusted(true);
  window->EnqueueAnimationFrameEvent(*this, *event);
}

void HTMLDialogElement::CreateCloseWatcher() {
  if (!GetDocument().DefaultView()) {
    return;
  }
  close_watcher_ = ++last_close_watcher_;
  close_watcher_enabled_ = false;
  SetCloseWatcherEnabledState();
}

void HTMLDialogElement::DestroyCloseWatcher() {
  close_watcher_ = 0;
  close_watcher_enabled_ = false;
}

void HTMLDialogElement::SetCloseWatcherEnabledState() {
  if (!IsOpenAndActive() || !close_watcher_) {
    return;
  }
  close_watcher_enabled_ = ClosedByStateOf() != ClosedByState::kNone;
}

}  // namespace mitto
