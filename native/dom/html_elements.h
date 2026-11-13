// The elements of the HTML Standard whose classes have nothing of their own
// yet beyond their interface: each member of theirs that is not a reflected
// attribute is a Known Gap. The form controls among them get their behaviour
// with forms.
//
// Upstream: third_party/blink/renderer/core/html/html_tag_names.json5

#ifndef MITTO_DOM_HTML_ELEMENTS_H_
#define MITTO_DOM_HTML_ELEMENTS_H_

#include <string>

#include "mitto/dom/html_element.h"
#include "mitto/dom/html_media_element.h"

namespace mitto {

class HTMLBaseElement final : public HTMLElement {
 public:
  static HTMLBaseElement* Create(Document& document);

  explicit HTMLBaseElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLButtonElement final : public HTMLElement {
 public:
  static HTMLButtonElement* Create(Document& document);

  explicit HTMLButtonElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLDListElement final : public HTMLElement {
 public:
  static HTMLDListElement* Create(Document& document);

  explicit HTMLDListElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLDataElement final : public HTMLElement {
 public:
  static HTMLDataElement* Create(Document& document);

  explicit HTMLDataElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLDataListElement final : public HTMLElement {
 public:
  static HTMLDataListElement* Create(Document& document);

  explicit HTMLDataListElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLDetailsElement final : public HTMLElement {
 public:
  static HTMLDetailsElement* Create(Document& document);

  explicit HTMLDetailsElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLDialogElement final : public HTMLElement {
 public:
  static HTMLDialogElement* Create(Document& document);

  explicit HTMLDialogElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLDirectoryElement final : public HTMLElement {
 public:
  static HTMLDirectoryElement* Create(Document& document);

  explicit HTMLDirectoryElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLEmbedElement final : public HTMLElement {
 public:
  static HTMLEmbedElement* Create(Document& document);

  explicit HTMLEmbedElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLFieldSetElement final : public HTMLElement {
 public:
  static HTMLFieldSetElement* Create(Document& document);

  explicit HTMLFieldSetElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLFontElement final : public HTMLElement {
 public:
  static HTMLFontElement* Create(Document& document);

  explicit HTMLFontElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLFormElement final : public HTMLElement {
 public:
  static HTMLFormElement* Create(Document& document);

  explicit HTMLFormElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLFrameElement final : public HTMLElement {
 public:
  static HTMLFrameElement* Create(Document& document);

  explicit HTMLFrameElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLFrameSetElement final : public HTMLElement {
 public:
  static HTMLFrameSetElement* Create(Document& document);

  explicit HTMLFrameSetElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLHRElement final : public HTMLElement {
 public:
  static HTMLHRElement* Create(Document& document);

  explicit HTMLHRElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLIFrameElement final : public HTMLElement {
 public:
  static HTMLIFrameElement* Create(Document& document);

  explicit HTMLIFrameElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLLabelElement final : public HTMLElement {
 public:
  static HTMLLabelElement* Create(Document& document);

  explicit HTMLLabelElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLLegendElement final : public HTMLElement {
 public:
  static HTMLLegendElement* Create(Document& document);

  explicit HTMLLegendElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLMapElement final : public HTMLElement {
 public:
  static HTMLMapElement* Create(Document& document);

  explicit HTMLMapElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLMarqueeElement final : public HTMLElement {
 public:
  static HTMLMarqueeElement* Create(Document& document);

  explicit HTMLMarqueeElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLMenuElement final : public HTMLElement {
 public:
  static HTMLMenuElement* Create(Document& document);

  explicit HTMLMenuElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLMeterElement final : public HTMLElement {
 public:
  static HTMLMeterElement* Create(Document& document);

  explicit HTMLMeterElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

// The element of each of del, ins.
class HTMLModElement final : public HTMLElement {
 public:
  static HTMLModElement* Create(Document& document,
                                const std::u16string& local_name);

  HTMLModElement(Document& document, const std::u16string& local_name);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLObjectElement final : public HTMLElement {
 public:
  static HTMLObjectElement* Create(Document& document);

  explicit HTMLObjectElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLOptGroupElement final : public HTMLElement {
 public:
  static HTMLOptGroupElement* Create(Document& document);

  explicit HTMLOptGroupElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLOptionElement final : public HTMLElement {
 public:
  static HTMLOptionElement* Create(Document& document);

  explicit HTMLOptionElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLOutputElement final : public HTMLElement {
 public:
  static HTMLOutputElement* Create(Document& document);

  explicit HTMLOutputElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLParamElement final : public HTMLElement {
 public:
  static HTMLParamElement* Create(Document& document);

  explicit HTMLParamElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLPictureElement final : public HTMLElement {
 public:
  static HTMLPictureElement* Create(Document& document);

  explicit HTMLPictureElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLProgressElement final : public HTMLElement {
 public:
  static HTMLProgressElement* Create(Document& document);

  explicit HTMLProgressElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

// The element of each of blockquote, q.
class HTMLQuoteElement final : public HTMLElement {
 public:
  static HTMLQuoteElement* Create(Document& document,
                                  const std::u16string& local_name);

  HTMLQuoteElement(Document& document, const std::u16string& local_name);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLSelectElement final : public HTMLElement {
 public:
  static HTMLSelectElement* Create(Document& document);

  explicit HTMLSelectElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLSelectedContentElement final : public HTMLElement {
 public:
  static HTMLSelectedContentElement* Create(Document& document);

  explicit HTMLSelectedContentElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLSlotElement final : public HTMLElement {
 public:
  static HTMLSlotElement* Create(Document& document);

  explicit HTMLSlotElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLSourceElement final : public HTMLElement {
 public:
  static HTMLSourceElement* Create(Document& document);

  explicit HTMLSourceElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLTableCaptionElement final : public HTMLElement {
 public:
  static HTMLTableCaptionElement* Create(Document& document);

  explicit HTMLTableCaptionElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

// The element of each of td, th.
class HTMLTableCellElement final : public HTMLElement {
 public:
  static HTMLTableCellElement* Create(Document& document,
                                      const std::u16string& local_name);

  HTMLTableCellElement(Document& document, const std::u16string& local_name);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

// The element of each of col, colgroup.
class HTMLTableColElement final : public HTMLElement {
 public:
  static HTMLTableColElement* Create(Document& document,
                                     const std::u16string& local_name);

  HTMLTableColElement(Document& document, const std::u16string& local_name);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLTableElement final : public HTMLElement {
 public:
  static HTMLTableElement* Create(Document& document);

  explicit HTMLTableElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLTableRowElement final : public HTMLElement {
 public:
  static HTMLTableRowElement* Create(Document& document);

  explicit HTMLTableRowElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

// The element of each of tbody, tfoot, thead.
class HTMLTableSectionElement final : public HTMLElement {
 public:
  static HTMLTableSectionElement* Create(Document& document,
                                         const std::u16string& local_name);

  HTMLTableSectionElement(Document& document, const std::u16string& local_name);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLTextAreaElement final : public HTMLElement {
 public:
  static HTMLTextAreaElement* Create(Document& document);

  explicit HTMLTextAreaElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLTimeElement final : public HTMLElement {
 public:
  static HTMLTimeElement* Create(Document& document);

  explicit HTMLTimeElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLTrackElement final : public HTMLElement {
 public:
  static HTMLTrackElement* Create(Document& document);

  explicit HTMLTrackElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLVideoElement final : public HTMLMediaElement {
 public:
  static HTMLVideoElement* Create(Document& document);

  explicit HTMLVideoElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

// The elements behind a runtime-enabled feature, which the factory makes only
// while their feature is on.

class HTMLCameraElement final : public HTMLElement {
 public:
  static HTMLCameraElement* Create(Document& document);

  explicit HTMLCameraElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLCredentialElement final : public HTMLElement {
 public:
  static HTMLCredentialElement* Create(Document& document);

  explicit HTMLCredentialElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLFencedFrameElement final : public HTMLElement {
 public:
  static HTMLFencedFrameElement* Create(Document& document);

  explicit HTMLFencedFrameElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLGeolocationElement final : public HTMLElement {
 public:
  static HTMLGeolocationElement* Create(Document& document);

  explicit HTMLGeolocationElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLInstallElement final : public HTMLElement {
 public:
  static HTMLInstallElement* Create(Document& document);

  explicit HTMLInstallElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLLoginElement final : public HTMLElement {
 public:
  static HTMLLoginElement* Create(Document& document);

  explicit HTMLLoginElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLMenuBarElement final : public HTMLElement {
 public:
  static HTMLMenuBarElement* Create(Document& document);

  explicit HTMLMenuBarElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLMenuItemElement final : public HTMLElement {
 public:
  static HTMLMenuItemElement* Create(Document& document);

  explicit HTMLMenuItemElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLMenuListElement final : public HTMLElement {
 public:
  static HTMLMenuListElement* Create(Document& document);

  explicit HTMLMenuListElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLMicrophoneElement final : public HTMLElement {
 public:
  static HTMLMicrophoneElement* Create(Document& document);

  explicit HTMLMicrophoneElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLPersistentWidgetElement final : public HTMLElement {
 public:
  static HTMLPersistentWidgetElement* Create(Document& document);

  explicit HTMLPersistentWidgetElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLSubMenuElement final : public HTMLElement {
 public:
  static HTMLSubMenuElement* Create(Document& document);

  explicit HTMLSubMenuElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLUserMediaElement final : public HTMLElement {
 public:
  static HTMLUserMediaElement* Create(Document& document);

  explicit HTMLUserMediaElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_ELEMENTS_H_
