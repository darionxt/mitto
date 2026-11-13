// The DOM's HTMLElement: an element of the HTML namespace, and the class of
// those whose name has no interface of its own, such as section or b.
//
// Upstream: third_party/blink/renderer/core/html/html_element.h
// Upstream: third_party/blink/renderer/core/html/html_element.cc

#ifndef MITTO_DOM_HTML_ELEMENT_H_
#define MITTO_DOM_HTML_ELEMENT_H_

#include <string>

#include "mitto/dom/element.h"

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
  std::u16string ContentEditable() const;
  void SetContentEditable(const std::u16string& value,
                          bindings::ExceptionState& exception_state);
  // Whether the element is editable: its own or the nearest HTML ancestor's
  // contenteditable makes it so, or the designMode of the document it is
  // connected to does.
  bool IsContentEditableForBinding() const;

  // Element's: the summary of its parent details, its first summary child,
  // is focusable, as Blink's HTMLSummaryElement is.
  int32_t DefaultTabIndex() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_ELEMENT_H_
