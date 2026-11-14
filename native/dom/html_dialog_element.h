// The HTML Standard's dialog element: show(), showModal(), close() and
// requestClose() with their beforetoggle, toggle, cancel and close events, as
// Blink's HTMLDialogElement fires them. Without layout, focus or input, a
// modal one is only modal for :modal: it puts nothing in a top layer, makes
// nothing inert, moves no focus, and no Escape key or click outside closes
// it.
//
// Upstream: third_party/blink/renderer/core/html/html_dialog_element.h
// Upstream: third_party/blink/renderer/core/html/html_dialog_element.cc
// Upstream: third_party/blink/renderer/core/html/closewatcher/close_watcher.cc

#ifndef MITTO_DOM_HTML_DIALOG_ELEMENT_H_
#define MITTO_DOM_HTML_DIALOG_ELEMENT_H_

#include <stdint.h>

#include <optional>
#include <string>

#include "mitto/dom/html_element.h"
#include "mitto/dom/qualified_name.h"
#include "v8/include/cppgc/member.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class ToggleEvent;

class HTMLDialogElement final : public HTMLElement {
 public:
  static HTMLDialogElement* Create(Document& document);

  explicit HTMLDialogElement(Document& document);
  ~HTMLDialogElement() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  const std::u16string& ReturnValue() const { return return_value_; }
  void SetReturnValue(const std::u16string& value) { return_value_ = value; }
  std::u16string ClosedBy() const;
  void SetClosedBy(const std::u16string& value);
  void Show(bindings::ExceptionState& exception_state);
  void ShowModal(bindings::ExceptionState& exception_state);
  void Close(const std::optional<std::u16string>& return_value);
  void RequestClose(const std::optional<std::u16string>& return_value,
                    bindings::ExceptionState& exception_state);

  // Whether it has the open attribute.
  bool IsOpen() const;
  // Whether showModal() opened it, which :modal matches, until close() or a
  // removal from its document, though the open attribute may go first.
  bool IsModal() const { return is_modal_; }

  // Node:
  InsertionNotificationRequest InsertedInto(Node& insertion_point) override;
  void RemovedFrom(Node& insertion_point) override;
  // Element:
  void AttributeChanged(const QualifiedName& name,
                        const std::u16string* old_value,
                        const std::u16string* new_value) override;

 private:
  enum class ClosedByState { kAny, kCloseRequest, kNone };

  ClosedByState ClosedByStateOf() const;
  // Blink's IsOpenAndActive(): open, connected and in a document that has a
  // window, as only such a document of a Page is fully active.
  bool IsOpenAndActive() const;
  // Fires beforetoggle, cancelable when opening, and returns false if that
  // was canceled or, when opening, the dialog opened meanwhile or a modal one
  // left its document. Otherwise queues toggle as Blink does: in a task of
  // the DOM manipulation task source that replaces the one still to fire and
  // keeps its old state.
  bool DispatchToggleEvents(bool opening, bool as_modal);
  void DispatchPendingToggleEvent();
  // close fires at the next animation frame, before its callbacks, and only
  // for a document that has a window.
  void ScheduleCloseEvent();

  // The close watcher that Blink makes while the dialog is open in a
  // connected tree of a document that has a window. Only requestClose() asks
  // it, as nothing here presses Escape or clicks outside, so the watcher is
  // kept as its number and whether it is enabled. A watcher that the dialog
  // replaced, such as by a close() and show() in a cancel listener, cannot
  // close it.
  void CreateCloseWatcher();
  void DestroyCloseWatcher();
  // Blink's SetCloseWatcherEnabledState(): enabled unless closedBy is none.
  void SetCloseWatcherEnabledState();

  std::u16string return_value_;
  std::optional<std::u16string> request_close_return_value_;
  bool is_modal_ = false;
  // close() runs, so that removing the open attribute does nothing else.
  bool is_closing_ = false;
  cppgc::Member<ToggleEvent> pending_toggle_event_;
  // Which queued task fires the pending event: only the last one queued.
  uint64_t toggle_task_ = 0;
  // The close watcher's number, 0 for none, and the last one made.
  uint64_t close_watcher_ = 0;
  uint64_t last_close_watcher_ = 0;
  bool close_watcher_enabled_ = false;
  // The watcher whose cancel event fires, which a requestClose() from its
  // listeners does not ask again.
  uint64_t close_watcher_dispatching_cancel_ = 0;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_DIALOG_ELEMENT_H_
