// The elements of the HTML Standard whose classes have little of their own
// beyond their interface: their reflected attributes, with those that Blink's
// classes read by hand, such as a form control's formAction and a meter's
// numbers. Each other member of theirs is a Known Gap. The form controls among
// them get their behaviour with forms.
//
// Upstream: third_party/blink/renderer/core/html/html_tag_names.json5

#ifndef MITTO_DOM_HTML_ELEMENTS_H_
#define MITTO_DOM_HTML_ELEMENTS_H_

#include <stdint.h>

#include <optional>
#include <string>

#include "mitto/dom/html_element.h"
#include "mitto/dom/html_media_element.h"

namespace mitto {

class HTMLButtonElement final : public HTMLElement {
 public:
  static HTMLButtonElement* Create(Document& document);

  explicit HTMLButtonElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  int32_t DefaultTabIndex() const override;
  // Node's: activation behaviour, as Blink's button has, though it submits
  // no form yet, so that a click inside it leaves a summary alone.
  bool HasActivationBehavior() const override;

  // The members of the interface beyond its reflected attributes.
  //
  // submit, reset or button: the type attribute's value in any case, else
  // button while a command or commandfor attribute came since the type
  // attribute last changed, as Blink's ParseAttribute() keeps type_, else
  // submit.
  std::u16string Type() const;
  void SetType(const std::u16string& type);
  std::u16string FormAction() const;
  void SetFormAction(const std::u16string& action);
  std::u16string FormEnctype() const;
  void SetFormEnctype(const std::u16string& enctype);
  std::u16string FormMethod() const;
  void SetFormMethod(const std::u16string& method);

  // Element:
  void AttributeChanged(const QualifiedName& name,
                        const std::u16string* old_value,
                        const std::u16string* new_value) override;

 private:
  bool command_seen_ = false;
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

  // The members of the interface beyond its reflected attributes: the URL
  // that src reflects, as Blink's GetURLAttribute() resolves it.
  std::u16string Src() const { return ReflectedURLAttribute(u"src"); }
  void SetSrc(const std::u16string& src,
              bindings::ExceptionState& exception_state);
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
  DOMTokenList* RelList() {
    return EnsureTokenList(u"rel", DOMTokenList::SupportedTokens::kRel);
  }

  // The members of the interface beyond its reflected attributes, which an
  // attribute that is missing or that is not one of their values gives as
  // get, application/x-www-form-urlencoded and the document's URL.
  std::u16string Method() const;
  void SetMethod(const std::u16string& method);
  std::u16string Enctype() const;
  void SetEnctype(const std::u16string& enctype);
  std::u16string Encoding() const { return Enctype(); }
  void SetEncoding(const std::u16string& encoding) { SetEnctype(encoding); }
  std::u16string Action() const;
  void SetAction(const std::u16string& action);
};

class HTMLFrameElement final : public HTMLElement {
 public:
  static HTMLFrameElement* Create(Document& document);

  explicit HTMLFrameElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  int32_t DefaultTabIndex() const override;
};

class HTMLFrameSetElement final : public HTMLElement {
 public:
  static HTMLFrameSetElement* Create(Document& document);

  explicit HTMLFrameSetElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

 protected:
  // HTMLElement's: the window's handlers of Blink's
  // HTMLFrameSetElement::ParseAttribute().
  std::u16string_view WindowEventNameForAttributeName(
      const QualifiedName& name) const override;
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
  int32_t DefaultTabIndex() const override;
  DOMTokenList* Sandbox() {
    return EnsureTokenList(u"sandbox", DOMTokenList::SupportedTokens::kSandbox);
  }

  std::u16string Srcdoc() const;
  void SetSrcdoc(const std::u16string& srcdoc,
                 bindings::ExceptionState& exception_state);
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

  // The content attribute's non-negative integer, else 6 and 85.
  uint32_t ScrollAmount() const;
  void SetScrollAmount(uint32_t amount);
  uint32_t ScrollDelay() const;
  void SetScrollDelay(uint32_t delay);
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

  // The members of the interface: each number of its content attribute,
  // clamped to the others as Blink's meter clamps it.
  double Min() const;
  void SetMin(double min);
  double Max() const;
  void SetMax(double max);
  double Value() const;
  void SetValue(double value);
  double Low() const;
  void SetLow(double low);
  double High() const;
  void SetHigh(double high);
  double Optimum() const;
  void SetOptimum(double optimum);
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
  int32_t DefaultTabIndex() const override;

  // The members of the interface beyond its reflected attributes: the URLs
  // that data and codebase reflect.
  std::u16string Data() const { return ReflectedURLAttribute(u"data"); }
  void SetData(const std::u16string& data,
               bindings::ExceptionState& exception_state);
  std::u16string CodeBase() const { return ReflectedURLAttribute(u"codebase"); }
  void SetCodeBase(const std::u16string& code_base,
                   bindings::ExceptionState& exception_state);
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

  // new Option(data, value, defaultSelected, selected), in the current
  // window's document: a text child of the data, the value and selected
  // attributes, as Blink's CreateForJSConstructor() makes it. Its
  // selectedness comes with forms.
  static HTMLOptionElement* CreateForJSConstructor(
      const std::u16string& data,
      const std::optional<std::u16string>& value,
      bool default_selected,
      bool selected);

  explicit HTMLOptionElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface beyond its reflected attributes: each is
  // its content attribute, or the option's text while that is missing.
  std::u16string Label() const;
  void SetLabel(const std::u16string& label);
  std::u16string Value() const;
  void SetValue(const std::u16string& value);

 private:
  // CollectOptionInnerText() with its HTML spaces stripped and collapsed.
  std::u16string CollapsedText() const;
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

  // The members of the interface: max is the positive number of its content
  // attribute, else 1, and value the number of its own, at least 0 and at
  // most max. Setting max to 0 or less does nothing.
  double Max() const;
  void SetMax(double max);
  double Value() const;
  void SetValue(double value);
  // value / max, or -1 for a progress of no value attribute.
  double Position() const;
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
  int32_t DefaultTabIndex() const override;

  std::u16string IDLExposedAutofillValue() const;
  void SetIDLExposedAutofillValue(const std::u16string& value);
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

  std::u16string Type() const;
  void SetType(const std::u16string& type);
};

class HTMLTableCaptionElement final : public HTMLElement {
 public:
  static HTMLTableCaptionElement* Create(Document& document);

  explicit HTMLTableCaptionElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class HTMLTextAreaElement final : public HTMLElement {
 public:
  static HTMLTextAreaElement* Create(Document& document);

  explicit HTMLTextAreaElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  int32_t DefaultTabIndex() const override;

  // The members of the interface beyond its reflected attributes: cols and
  // rows are the content attribute's positive integer, else 20 and 2, which
  // setting 0 writes.
  uint32_t Cols() const;
  void SetCols(uint32_t cols);
  uint32_t Rows() const;
  void SetRows(uint32_t rows);
  int32_t MaxLength() const;
  void SetMaxLength(int32_t length, bindings::ExceptionState& exception_state);
  int32_t MinLength() const;
  void SetMinLength(int32_t length, bindings::ExceptionState& exception_state);
  std::u16string IDLExposedAutofillValue() const;
  void SetIDLExposedAutofillValue(const std::u16string& value);
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

  // subtitles while the kind attribute is missing, one of the five kinds of
  // text track it names in any case, else metadata.
  std::u16string Kind() const;
  void SetKind(const std::u16string& kind);
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
