// The HTML Standard's details element: opening or closing it fires a toggle
// event in a task, a click on its first summary opens or closes it, and
// opening one of a name closes the others of that name in its tree, as
// Blink's HTMLDetailsElement does. Without layout, its contents are not
// hidden from anything.
//
// Upstream: third_party/blink/renderer/core/html/html_details_element.h
// Upstream: third_party/blink/renderer/core/html/html_details_element.cc

#ifndef MITTO_DOM_HTML_DETAILS_ELEMENT_H_
#define MITTO_DOM_HTML_DETAILS_ELEMENT_H_

#include <stdint.h>

#include <string>
#include <vector>

#include "mitto/dom/html_element.h"
#include "mitto/dom/qualified_name.h"
#include "v8/include/cppgc/member.h"

namespace mitto {

class ToggleEvent;

class HTMLDetailsElement final : public HTMLElement {
 public:
  static HTMLDetailsElement* Create(Document& document);

  explicit HTMLDetailsElement(Document& document);
  ~HTMLDetailsElement() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // Whether it has the open attribute.
  bool IsOpen() const;
  // Opens it if it is closed and closes it if it is open, as Blink's
  // ToggleOpen() sets the open attribute.
  void ToggleOpen();
  // Its first summary child, which a click opens or closes it by, or null.
  Element* MainSummary() const;

  // Node:
  InsertionNotificationRequest InsertedInto(Node& insertion_point) override;
  // Element:
  void AttributeChanged(const QualifiedName& name,
                        const std::u16string* old_value,
                        const std::u16string* new_value) override;

 protected:
  // Node: Blink's copy sets its attributes as the parser does, so an open one
  // fires a toggle event too.
  Node* CloneWithoutChildren(Document& document) const override;

 private:
  // Blink's ParseAttribute() for open: a toggle event in a task of the DOM
  // manipulation task source, which replaces the one still to fire and keeps
  // its old state, as Blink posts a new task and cancels the one before.
  void QueueToggleEvent(bool is_open);
  void DispatchPendingEvent();
  // Blink's MaybeCloseForExclusivity(): an open one that joins a name group
  // with another open one closes itself.
  void MaybeCloseForExclusivity();
  // The other details elements of its name in its tree, in tree order, none
  // for no name.
  std::vector<HTMLDetailsElement*> OtherElementsInNameGroup();
  std::u16string Name() const;

  cppgc::Member<ToggleEvent> pending_toggle_event_;
  // Which queued task fires the pending event: only the last one queued.
  uint64_t toggle_task_ = 0;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_DETAILS_ELEMENT_H_
