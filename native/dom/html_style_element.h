// The style element. The Runtime has no style yet (roadmap, M9): what it
// keeps of the element's sheet is its @import rules, which it fetches as
// Blink's StyleElement does, holding back scripts, rendering or the parser as
// the element's place says, and it fires load or error at the element once
// they have all ended.
//
// Upstream: third_party/blink/renderer/core/html/html_style_element.h
// Upstream: third_party/blink/renderer/core/html/html_style_element.cc
// Upstream: third_party/blink/renderer/core/css/style_element.cc
// Upstream: third_party/blink/renderer/core/css/style_engine.cc

#ifndef MITTO_DOM_HTML_STYLE_ELEMENT_H_
#define MITTO_DOM_HTML_STYLE_ELEMENT_H_

#include <string>

#include "mitto/dom/document.h"
#include "mitto/dom/html_element.h"
#include "mitto/dom/style_sheet_contents.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class HTMLStyleElement final : public HTMLElement, public StyleSheetOwner {
 public:
  static HTMLStyleElement* Create(Document& document);

  explicit HTMLStyleElement(Document& document);
  ~HTMLStyleElement() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  DOMTokenList* Blocking() {
    return EnsureTokenList(u"blocking",
                           DOMTokenList::SupportedTokens::kBlocking);
  }

  // The parser made it: its sheet waits for the end of its text, and blocks
  // scripts.
  void SetCreatedByParser();
  // FinishParsingChildren(): the parser has put in all of its text.
  void FinishParsingChildren();

  // Node:
  InsertionNotificationRequest InsertedInto(Node& insertion_point) override;
  void RemovedFrom(Node& insertion_point) override;
  void ChildrenChanged(ChildrenChangeType type,
                       Node& child,
                       bool by_parser) override;
  // Element:
  void AttributeChanged(const QualifiedName& name,
                        const std::u16string* old_value,
                        const std::u16string* new_value) override;
  // StyleSheetOwner:
  void SheetLoaded(bool error) override;

 private:
  // StyleElement::Process(): a sheet of its text, in place of the one it
  // had, for an element in a document.
  void Process();
  // StyleElement::ClearSheet().
  void ClearSheet();
  // Whether its sheet is being made, or an import of it still loads.
  bool IsLoading() const;
  // A blocking=render element, or one that the parser made.
  bool IsPotentiallyRenderBlocking() const;
  // NotifyLoadedSheetAndAllCriticalSubresources(): load or error at the
  // element from a task, which holds the load event back.
  void ScheduleEvent(bool loaded);

  cppgc::Member<StyleSheetContents> sheet_;
  Document::PendingSheetType pending_sheet_type_ =
      Document::PendingSheetType::kNone;
  bool created_by_parser_ = false;
  bool has_finished_parsing_children_ = true;
  bool loading_ = false;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_STYLE_ELEMENT_H_
