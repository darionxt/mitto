#include "mitto/dom/html_elements.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>

#include "base/strings/string_util.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_button_element.h"
#include "mitto/bindings/v8_html_camera_element.h"
#include "mitto/bindings/v8_html_credential_element.h"
#include "mitto/bindings/v8_html_data_element.h"
#include "mitto/bindings/v8_html_data_list_element.h"
#include "mitto/bindings/v8_html_directory_element.h"
#include "mitto/bindings/v8_html_dlist_element.h"
#include "mitto/bindings/v8_html_embed_element.h"
#include "mitto/bindings/v8_html_fenced_frame_element.h"
#include "mitto/bindings/v8_html_field_set_element.h"
#include "mitto/bindings/v8_html_font_element.h"
#include "mitto/bindings/v8_html_form_element.h"
#include "mitto/bindings/v8_html_frame_element.h"
#include "mitto/bindings/v8_html_frame_set_element.h"
#include "mitto/bindings/v8_html_geolocation_element.h"
#include "mitto/bindings/v8_html_hr_element.h"
#include "mitto/bindings/v8_html_iframe_element.h"
#include "mitto/bindings/v8_html_install_element.h"
#include "mitto/bindings/v8_html_label_element.h"
#include "mitto/bindings/v8_html_legend_element.h"
#include "mitto/bindings/v8_html_login_element.h"
#include "mitto/bindings/v8_html_map_element.h"
#include "mitto/bindings/v8_html_marquee_element.h"
#include "mitto/bindings/v8_html_menu_bar_element.h"
#include "mitto/bindings/v8_html_menu_element.h"
#include "mitto/bindings/v8_html_menu_item_element.h"
#include "mitto/bindings/v8_html_menu_list_element.h"
#include "mitto/bindings/v8_html_meter_element.h"
#include "mitto/bindings/v8_html_microphone_element.h"
#include "mitto/bindings/v8_html_mod_element.h"
#include "mitto/bindings/v8_html_object_element.h"
#include "mitto/bindings/v8_html_opt_group_element.h"
#include "mitto/bindings/v8_html_option_element.h"
#include "mitto/bindings/v8_html_output_element.h"
#include "mitto/bindings/v8_html_param_element.h"
#include "mitto/bindings/v8_html_persistent_widget_element.h"
#include "mitto/bindings/v8_html_picture_element.h"
#include "mitto/bindings/v8_html_progress_element.h"
#include "mitto/bindings/v8_html_quote_element.h"
#include "mitto/bindings/v8_html_select_element.h"
#include "mitto/bindings/v8_html_selected_content_element.h"
#include "mitto/bindings/v8_html_slot_element.h"
#include "mitto/bindings/v8_html_source_element.h"
#include "mitto/bindings/v8_html_sub_menu_element.h"
#include "mitto/bindings/v8_html_table_caption_element.h"
#include "mitto/bindings/v8_html_text_area_element.h"
#include "mitto/bindings/v8_html_time_element.h"
#include "mitto/bindings/v8_html_track_element.h"
#include "mitto/bindings/v8_html_user_media_element.h"
#include "mitto/bindings/v8_html_video_element.h"
#include "mitto/dom/character_data.h"
#include "mitto/dom/document.h"
#include "mitto/dom/event_handler_attributes.h"
#include "mitto/dom/form_control_attributes.h"
#include "mitto/dom/html_image_element.h"
#include "mitto/dom/html_names.h"
#include "mitto/dom/html_parser_idioms.h"
#include "mitto/dom/media_query_list.h"
#include "mitto/dom/text.h"
#include "mitto/dom/window.h"

namespace mitto {

// static
HTMLButtonElement* HTMLButtonElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLButtonElement>(document);
}

HTMLButtonElement::HTMLButtonElement(Document& document)
    : HTMLElement(document, u"button") {}

const bindings::WrapperTypeInfo* HTMLButtonElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLButtonElement::kWrapperTypeInfo;
}

bool HTMLButtonElement::HasActivationBehavior() const {
  return true;
}

int32_t HTMLButtonElement::DefaultTabIndex() const {
  return 0;
}

// static
HTMLDListElement* HTMLDListElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLDListElement>(document);
}

HTMLDListElement::HTMLDListElement(Document& document)
    : HTMLElement(document, u"dl") {}

const bindings::WrapperTypeInfo* HTMLDListElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLDListElement::kWrapperTypeInfo;
}

// static
HTMLDataElement* HTMLDataElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLDataElement>(document);
}

HTMLDataElement::HTMLDataElement(Document& document)
    : HTMLElement(document, u"data") {}

const bindings::WrapperTypeInfo* HTMLDataElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLDataElement::kWrapperTypeInfo;
}

// static
HTMLDataListElement* HTMLDataListElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLDataListElement>(document);
}

HTMLDataListElement::HTMLDataListElement(Document& document)
    : HTMLElement(document, u"datalist") {}

const bindings::WrapperTypeInfo* HTMLDataListElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLDataListElement::kWrapperTypeInfo;
}

// static
HTMLDirectoryElement* HTMLDirectoryElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLDirectoryElement>(document);
}

HTMLDirectoryElement::HTMLDirectoryElement(Document& document)
    : HTMLElement(document, u"dir") {}

const bindings::WrapperTypeInfo* HTMLDirectoryElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLDirectoryElement::kWrapperTypeInfo;
}

// static
HTMLEmbedElement* HTMLEmbedElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLEmbedElement>(document);
}

HTMLEmbedElement::HTMLEmbedElement(Document& document)
    : HTMLElement(document, u"embed") {}

const bindings::WrapperTypeInfo* HTMLEmbedElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLEmbedElement::kWrapperTypeInfo;
}

Element::NamedItemType HTMLEmbedElement::GetNamedItemType() const {
  return NamedItemType::kName;
}

// static
HTMLFieldSetElement* HTMLFieldSetElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLFieldSetElement>(document);
}

HTMLFieldSetElement::HTMLFieldSetElement(Document& document)
    : HTMLElement(document, u"fieldset") {}

const bindings::WrapperTypeInfo* HTMLFieldSetElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLFieldSetElement::kWrapperTypeInfo;
}

// static
HTMLFontElement* HTMLFontElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLFontElement>(document);
}

HTMLFontElement::HTMLFontElement(Document& document)
    : HTMLElement(document, u"font") {}

const bindings::WrapperTypeInfo* HTMLFontElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLFontElement::kWrapperTypeInfo;
}

// static
HTMLFormElement* HTMLFormElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLFormElement>(document);
}

HTMLFormElement::HTMLFormElement(Document& document)
    : HTMLElement(document, u"form") {}

const bindings::WrapperTypeInfo* HTMLFormElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLFormElement::kWrapperTypeInfo;
}

Element::NamedItemType HTMLFormElement::GetNamedItemType() const {
  return NamedItemType::kName;
}

// static
HTMLFrameElement* HTMLFrameElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLFrameElement>(document);
}

HTMLFrameElement::HTMLFrameElement(Document& document)
    : HTMLFrameOwnerElement(document, u"frame") {}

const bindings::WrapperTypeInfo* HTMLFrameElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLFrameElement::kWrapperTypeInfo;
}

int32_t HTMLFrameElement::DefaultTabIndex() const {
  return 0;
}

// static
HTMLFrameSetElement* HTMLFrameSetElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLFrameSetElement>(document);
}

HTMLFrameSetElement::HTMLFrameSetElement(Document& document)
    : HTMLElement(document, u"frameset") {}

const bindings::WrapperTypeInfo* HTMLFrameSetElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLFrameSetElement::kWrapperTypeInfo;
}

std::u16string_view HTMLFrameSetElement::WindowEventNameForAttributeName(
    const QualifiedName& name) const {
  return FrameSetWindowEventNameForAttributeName(name);
}

// static
HTMLHRElement* HTMLHRElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLHRElement>(document);
}

HTMLHRElement::HTMLHRElement(Document& document)
    : HTMLElement(document, u"hr") {}

const bindings::WrapperTypeInfo* HTMLHRElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLHRElement::kWrapperTypeInfo;
}

// static
HTMLIFrameElement* HTMLIFrameElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLIFrameElement>(document);
}

HTMLIFrameElement::HTMLIFrameElement(Document& document)
    : HTMLFrameOwnerElement(document, u"iframe") {}

const bindings::WrapperTypeInfo* HTMLIFrameElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLIFrameElement::kWrapperTypeInfo;
}

Element::NamedItemType HTMLIFrameElement::GetNamedItemType() const {
  return NamedItemType::kName;
}

int32_t HTMLIFrameElement::DefaultTabIndex() const {
  return 0;
}

// static
HTMLLabelElement* HTMLLabelElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLLabelElement>(document);
}

HTMLLabelElement::HTMLLabelElement(Document& document)
    : HTMLElement(document, u"label") {}

const bindings::WrapperTypeInfo* HTMLLabelElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLLabelElement::kWrapperTypeInfo;
}

// static
HTMLLegendElement* HTMLLegendElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLLegendElement>(document);
}

HTMLLegendElement::HTMLLegendElement(Document& document)
    : HTMLElement(document, u"legend") {}

const bindings::WrapperTypeInfo* HTMLLegendElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLLegendElement::kWrapperTypeInfo;
}

// static
HTMLMapElement* HTMLMapElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLMapElement>(document);
}

HTMLMapElement::HTMLMapElement(Document& document)
    : HTMLElement(document, u"map") {}

const bindings::WrapperTypeInfo* HTMLMapElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLMapElement::kWrapperTypeInfo;
}

// static
HTMLMarqueeElement* HTMLMarqueeElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLMarqueeElement>(document);
}

HTMLMarqueeElement::HTMLMarqueeElement(Document& document)
    : HTMLElement(document, u"marquee") {}

const bindings::WrapperTypeInfo* HTMLMarqueeElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLMarqueeElement::kWrapperTypeInfo;
}

// static
HTMLMenuElement* HTMLMenuElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLMenuElement>(document);
}

HTMLMenuElement::HTMLMenuElement(Document& document)
    : HTMLElement(document, u"menu") {}

const bindings::WrapperTypeInfo* HTMLMenuElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLMenuElement::kWrapperTypeInfo;
}

// static
HTMLMeterElement* HTMLMeterElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLMeterElement>(document);
}

HTMLMeterElement::HTMLMeterElement(Document& document)
    : HTMLElement(document, u"meter") {}

const bindings::WrapperTypeInfo* HTMLMeterElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLMeterElement::kWrapperTypeInfo;
}

// static
HTMLModElement* HTMLModElement::Create(Document& document,
                                       const std::u16string& local_name) {
  return bindings::MakeGarbageCollected<HTMLModElement>(document, local_name);
}

HTMLModElement::HTMLModElement(Document& document,
                               const std::u16string& local_name)
    : HTMLElement(document, local_name) {}

const bindings::WrapperTypeInfo* HTMLModElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLModElement::kWrapperTypeInfo;
}

// static
HTMLObjectElement* HTMLObjectElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLObjectElement>(document);
}

HTMLObjectElement::HTMLObjectElement(Document& document)
    : HTMLElement(document, u"object") {}

const bindings::WrapperTypeInfo* HTMLObjectElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLObjectElement::kWrapperTypeInfo;
}

Element::NamedItemType HTMLObjectElement::GetNamedItemType() const {
  return NamedItemType::kNameOrId;
}

int32_t HTMLObjectElement::DefaultTabIndex() const {
  return 0;
}

// static
HTMLOptGroupElement* HTMLOptGroupElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLOptGroupElement>(document);
}

HTMLOptGroupElement::HTMLOptGroupElement(Document& document)
    : HTMLElement(document, u"optgroup") {}

const bindings::WrapperTypeInfo* HTMLOptGroupElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLOptGroupElement::kWrapperTypeInfo;
}

// static
HTMLOptionElement* HTMLOptionElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLOptionElement>(document);
}

HTMLOptionElement::HTMLOptionElement(Document& document)
    : HTMLElement(document, u"option") {}

const bindings::WrapperTypeInfo* HTMLOptionElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLOptionElement::kWrapperTypeInfo;
}

// static
HTMLOutputElement* HTMLOutputElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLOutputElement>(document);
}

HTMLOutputElement::HTMLOutputElement(Document& document)
    : HTMLElement(document, u"output") {}

const bindings::WrapperTypeInfo* HTMLOutputElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLOutputElement::kWrapperTypeInfo;
}

// static
HTMLParamElement* HTMLParamElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLParamElement>(document);
}

HTMLParamElement::HTMLParamElement(Document& document)
    : HTMLElement(document, u"param") {}

const bindings::WrapperTypeInfo* HTMLParamElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLParamElement::kWrapperTypeInfo;
}

// static
HTMLPictureElement* HTMLPictureElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLPictureElement>(document);
}

HTMLPictureElement::HTMLPictureElement(Document& document)
    : HTMLElement(document, u"picture") {}

const bindings::WrapperTypeInfo* HTMLPictureElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLPictureElement::kWrapperTypeInfo;
}

// Upstream: third_party/blink/renderer/core/html/html_picture_element.cc
void HTMLPictureElement::SourceChanged() {
  for (Node* child = FirstChild(); child; child = child->NextSibling()) {
    if (child->IsElementNode() &&
        static_cast<Element*>(child)->HasHTMLTagName(u"img")) {
      static_cast<HTMLImageElement*>(child)->SelectSourceURL(
          ImageLoader::kUpdateNormal);
    }
  }
}

// static
HTMLProgressElement* HTMLProgressElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLProgressElement>(document);
}

HTMLProgressElement::HTMLProgressElement(Document& document)
    : HTMLElement(document, u"progress") {}

const bindings::WrapperTypeInfo* HTMLProgressElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLProgressElement::kWrapperTypeInfo;
}

// static
HTMLQuoteElement* HTMLQuoteElement::Create(Document& document,
                                           const std::u16string& local_name) {
  return bindings::MakeGarbageCollected<HTMLQuoteElement>(document, local_name);
}

HTMLQuoteElement::HTMLQuoteElement(Document& document,
                                   const std::u16string& local_name)
    : HTMLElement(document, local_name) {}

const bindings::WrapperTypeInfo* HTMLQuoteElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLQuoteElement::kWrapperTypeInfo;
}

// static
HTMLSelectElement* HTMLSelectElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLSelectElement>(document);
}

HTMLSelectElement::HTMLSelectElement(Document& document)
    : HTMLElement(document, u"select") {}

const bindings::WrapperTypeInfo* HTMLSelectElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLSelectElement::kWrapperTypeInfo;
}

int32_t HTMLSelectElement::DefaultTabIndex() const {
  return 0;
}

// static
HTMLSelectedContentElement* HTMLSelectedContentElement::Create(
    Document& document) {
  return bindings::MakeGarbageCollected<HTMLSelectedContentElement>(document);
}

HTMLSelectedContentElement::HTMLSelectedContentElement(Document& document)
    : HTMLElement(document, u"selectedcontent") {}

const bindings::WrapperTypeInfo*
HTMLSelectedContentElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLSelectedContentElement::kWrapperTypeInfo;
}

// static
HTMLSlotElement* HTMLSlotElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLSlotElement>(document);
}

HTMLSlotElement::HTMLSlotElement(Document& document)
    : HTMLElement(document, u"slot") {}

const bindings::WrapperTypeInfo* HTMLSlotElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLSlotElement::kWrapperTypeInfo;
}

// static
HTMLSourceElement* HTMLSourceElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLSourceElement>(document);
}

HTMLSourceElement::HTMLSourceElement(Document& document)
    : HTMLElement(document, u"source") {}

const bindings::WrapperTypeInfo* HTMLSourceElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLSourceElement::kWrapperTypeInfo;
}

// static
HTMLTableCaptionElement* HTMLTableCaptionElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLTableCaptionElement>(document);
}

HTMLTableCaptionElement::HTMLTableCaptionElement(Document& document)
    : HTMLElement(document, u"caption") {}

const bindings::WrapperTypeInfo* HTMLTableCaptionElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLTableCaptionElement::kWrapperTypeInfo;
}

// static
HTMLTextAreaElement* HTMLTextAreaElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLTextAreaElement>(document);
}

HTMLTextAreaElement::HTMLTextAreaElement(Document& document)
    : HTMLElement(document, u"textarea") {}

const bindings::WrapperTypeInfo* HTMLTextAreaElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLTextAreaElement::kWrapperTypeInfo;
}

int32_t HTMLTextAreaElement::DefaultTabIndex() const {
  return 0;
}

// static
HTMLTimeElement* HTMLTimeElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLTimeElement>(document);
}

HTMLTimeElement::HTMLTimeElement(Document& document)
    : HTMLElement(document, u"time") {}

const bindings::WrapperTypeInfo* HTMLTimeElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLTimeElement::kWrapperTypeInfo;
}

// static
HTMLTrackElement* HTMLTrackElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLTrackElement>(document);
}

HTMLTrackElement::HTMLTrackElement(Document& document)
    : HTMLElement(document, u"track") {}

const bindings::WrapperTypeInfo* HTMLTrackElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLTrackElement::kWrapperTypeInfo;
}

// static
HTMLVideoElement* HTMLVideoElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLVideoElement>(document);
}

HTMLVideoElement::HTMLVideoElement(Document& document)
    : HTMLMediaElement(document, u"video") {}

const bindings::WrapperTypeInfo* HTMLVideoElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLVideoElement::kWrapperTypeInfo;
}

// static
HTMLCameraElement* HTMLCameraElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLCameraElement>(document);
}

HTMLCameraElement::HTMLCameraElement(Document& document)
    : HTMLElement(document, u"camera") {}

const bindings::WrapperTypeInfo* HTMLCameraElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLCameraElement::kWrapperTypeInfo;
}

// static
HTMLCredentialElement* HTMLCredentialElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLCredentialElement>(document);
}

HTMLCredentialElement::HTMLCredentialElement(Document& document)
    : HTMLElement(document, u"credential") {}

const bindings::WrapperTypeInfo* HTMLCredentialElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLCredentialElement::kWrapperTypeInfo;
}

// static
HTMLFencedFrameElement* HTMLFencedFrameElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLFencedFrameElement>(document);
}

HTMLFencedFrameElement::HTMLFencedFrameElement(Document& document)
    : HTMLElement(document, u"fencedframe") {}

const bindings::WrapperTypeInfo* HTMLFencedFrameElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLFencedFrameElement::kWrapperTypeInfo;
}

// static
HTMLGeolocationElement* HTMLGeolocationElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLGeolocationElement>(document);
}

HTMLGeolocationElement::HTMLGeolocationElement(Document& document)
    : HTMLElement(document, u"geolocation") {}

const bindings::WrapperTypeInfo* HTMLGeolocationElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLGeolocationElement::kWrapperTypeInfo;
}

// static
HTMLInstallElement* HTMLInstallElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLInstallElement>(document);
}

HTMLInstallElement::HTMLInstallElement(Document& document)
    : HTMLElement(document, u"install") {}

const bindings::WrapperTypeInfo* HTMLInstallElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLInstallElement::kWrapperTypeInfo;
}

// static
HTMLLoginElement* HTMLLoginElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLLoginElement>(document);
}

HTMLLoginElement::HTMLLoginElement(Document& document)
    : HTMLElement(document, u"login") {}

const bindings::WrapperTypeInfo* HTMLLoginElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLLoginElement::kWrapperTypeInfo;
}

// static
HTMLMenuBarElement* HTMLMenuBarElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLMenuBarElement>(document);
}

HTMLMenuBarElement::HTMLMenuBarElement(Document& document)
    : HTMLElement(document, u"menubar") {}

const bindings::WrapperTypeInfo* HTMLMenuBarElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLMenuBarElement::kWrapperTypeInfo;
}

// static
HTMLMenuItemElement* HTMLMenuItemElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLMenuItemElement>(document);
}

HTMLMenuItemElement::HTMLMenuItemElement(Document& document)
    : HTMLElement(document, u"menuitem") {}

const bindings::WrapperTypeInfo* HTMLMenuItemElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLMenuItemElement::kWrapperTypeInfo;
}

// static
HTMLMenuListElement* HTMLMenuListElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLMenuListElement>(document);
}

HTMLMenuListElement::HTMLMenuListElement(Document& document)
    : HTMLElement(document, u"menulist") {}

const bindings::WrapperTypeInfo* HTMLMenuListElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLMenuListElement::kWrapperTypeInfo;
}

// static
HTMLMicrophoneElement* HTMLMicrophoneElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLMicrophoneElement>(document);
}

HTMLMicrophoneElement::HTMLMicrophoneElement(Document& document)
    : HTMLElement(document, u"microphone") {}

const bindings::WrapperTypeInfo* HTMLMicrophoneElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLMicrophoneElement::kWrapperTypeInfo;
}

// static
HTMLPersistentWidgetElement* HTMLPersistentWidgetElement::Create(
    Document& document) {
  return bindings::MakeGarbageCollected<HTMLPersistentWidgetElement>(document);
}

HTMLPersistentWidgetElement::HTMLPersistentWidgetElement(Document& document)
    : HTMLElement(document, u"persistentwidget") {}

const bindings::WrapperTypeInfo*
HTMLPersistentWidgetElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLPersistentWidgetElement::kWrapperTypeInfo;
}

// static
HTMLSubMenuElement* HTMLSubMenuElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLSubMenuElement>(document);
}

HTMLSubMenuElement::HTMLSubMenuElement(Document& document)
    : HTMLElement(document, u"submenu") {}

const bindings::WrapperTypeInfo* HTMLSubMenuElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLSubMenuElement::kWrapperTypeInfo;
}

// static
HTMLUserMediaElement* HTMLUserMediaElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLUserMediaElement>(document);
}

HTMLUserMediaElement::HTMLUserMediaElement(Document& document)
    : HTMLElement(document, u"usermedia") {}

const bindings::WrapperTypeInfo* HTMLUserMediaElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLUserMediaElement::kWrapperTypeInfo;
}

namespace {

constexpr uint32_t kMaxReflectedUnsigned = 0x7fffffff;

// The content attribute's non-negative integer, as Blink's elements parse
// theirs, or `fallback` for one that is missing, does not parse, is 0 when
// `zero_is_default`, or is beyond 2147483647.
uint32_t UnsignedOr(const Element& element,
                    std::u16string_view name,
                    uint32_t fallback,
                    bool zero_is_default) {
  const std::u16string* const value = element.FindAttribute(name);
  const std::optional<uint32_t> number =
      value ? ParseHTMLNonNegativeInteger(*value) : std::nullopt;
  if (!number || *number > kMaxReflectedUnsigned ||
      (zero_is_default && !*number)) {
    return fallback;
  }
  return *number;
}

// SetUnsignedIntegralAttribute() with a default for a value beyond
// 2147483647.
void SetUnsignedOr(Element& element,
                   std::u16string_view name,
                   uint32_t value,
                   uint32_t fallback) {
  element.SetReflectedUnsignedIntegerAttribute(
      name, value > kMaxReflectedUnsigned ? fallback : value);
}

// ParseHTMLFloatingPointNumber() of the content attribute, or `fallback`.
double NumberOr(const Element& element,
                std::u16string_view name,
                double fallback) {
  const std::u16string* const value = element.FindAttribute(name);
  return value ? ParseHTMLFloatingPointNumber(*value).value_or(fallback)
               : fallback;
}

void SetNumber(Element& element, std::u16string_view name, double value) {
  element.SetAttributeWithoutValidation(name, SerializeForNumberType(value));
}

// A button's TypeFromString(): its type in lower case, or nothing.
std::optional<std::u16string_view> ButtonType(const std::u16string* value) {
  if (!value) {
    return std::nullopt;
  }
  for (const std::u16string_view type : {u"reset", u"button", u"submit"}) {
    if (base::EqualsCaseInsensitiveASCII(*value, type)) {
      return type;
    }
  }
  return std::nullopt;
}

}  // namespace

// HTMLButtonElement.

std::u16string HTMLButtonElement::Type() const {
  if (const std::optional<std::u16string_view> type =
          ButtonType(FindAttribute(u"type"))) {
    return std::u16string(*type);
  }
  return command_seen_ || FindAttribute(u"command") ||
                 FindAttribute(u"commandfor")
             ? u"button"
             : u"submit";
}

void HTMLButtonElement::SetType(const std::u16string& type) {
  SetAttributeWithoutValidation(u"type", type);
}

std::u16string HTMLButtonElement::FormAction() const {
  return FormActionOf(*this, u"formaction");
}

void HTMLButtonElement::SetFormAction(const std::u16string& action) {
  SetAttributeWithoutValidation(u"formaction", action);
}

std::u16string HTMLButtonElement::FormEnctype() const {
  return FormEnctypeOf(*this);
}

void HTMLButtonElement::SetFormEnctype(const std::u16string& enctype) {
  SetAttributeWithoutValidation(u"formenctype", enctype);
}

std::u16string HTMLButtonElement::FormMethod() const {
  return FormMethodOf(*this);
}

void HTMLButtonElement::SetFormMethod(const std::u16string& method) {
  SetAttributeWithoutValidation(u"formmethod", method);
}

namespace {

// The keywords of Blink's GetCommandEventType() that Chrome enables: the
// popover and dialog commands. Those of HTMLCommandActionsV2 and
// OverscrollAreas are behind flags that Chrome Releases leave off.
constexpr std::u16string_view kCommandKeywords[] = {
    u"toggle-popover", u"show-popover", u"hide-popover",
    u"close",          u"show-modal",   u"request-close"};

}  // namespace

bool HTMLButtonElement::CanBeCommandInvoker() const {
  // IsFormAssociatedSubmitButton(), of the form that it is in; the form
  // attribute comes with forms.
  if (Type() != u"submit") {
    return true;
  }
  for (Element* ancestor = ParentElement(); ancestor;
       ancestor = ancestor->ParentElement()) {
    if (ancestor->HasHTMLTagName(u"form")) {
      return false;
    }
  }
  return true;
}

std::u16string HTMLButtonElement::Command() const {
  if (!CanBeCommandInvoker()) {
    return std::u16string();
  }
  const std::u16string* const action = FindAttribute(u"command");
  if (!action || action->empty()) {
    return std::u16string();
  }
  if (action->starts_with(u"--")) {
    return *action;
  }
  for (const std::u16string_view keyword : kCommandKeywords) {
    if (base::EqualsCaseInsensitiveASCII(*action, keyword)) {
      return std::u16string(keyword);
    }
  }
  return std::u16string();
}

void HTMLButtonElement::SetCommand(const std::u16string& command) {
  SetAttributeWithoutValidation(u"command", command);
}

Element* HTMLButtonElement::CommandForElement() const {
  // [Reflect=commandfor]: the element, whatever the button is, as the
  // bindings give it in Blink, unlike HTMLElement::commandForElement(), which
  // a command's activation reads.
  return GetElementAttribute(u"commandfor");
}

void HTMLButtonElement::SetCommandForElement(Element* element) {
  SetElementAttribute(u"commandfor", element);
}

// A command or commandfor attribute makes a button of no valid type a button,
// which removing it leaves, until the type attribute changes.
void HTMLButtonElement::AttributeChanged(const QualifiedName& name,
                                         const std::u16string* old_value,
                                         const std::u16string* new_value) {
  HTMLElement::AttributeChanged(name, old_value, new_value);
  if (name.namespace_uri) {
    return;
  }
  if (name.local_name == u"type") {
    command_seen_ = false;
  } else if ((name.local_name == u"command" ||
              name.local_name == u"commandfor") &&
             new_value && !ButtonType(FindAttribute(u"type"))) {
    command_seen_ = true;
  }
}

// HTMLFormElement.

std::u16string HTMLFormElement::Method() const {
  const std::u16string* const value = FindAttribute(u"method");
  return std::u16string(ParseFormMethod(value ? *value : u""));
}

void HTMLFormElement::SetMethod(const std::u16string& method) {
  SetAttributeWithoutValidation(u"method", method);
}

std::u16string HTMLFormElement::Enctype() const {
  const std::u16string* const value = FindAttribute(u"enctype");
  return std::u16string(ParseFormEncodingType(value ? *value : u""));
}

void HTMLFormElement::SetEnctype(const std::u16string& enctype) {
  SetAttributeWithoutValidation(u"enctype", enctype);
}

std::u16string HTMLFormElement::Action() const {
  return FormActionOf(*this, u"action");
}

void HTMLFormElement::SetAction(const std::u16string& action) {
  SetAttributeWithoutValidation(u"action", action);
}

// HTMLTextAreaElement.

uint32_t HTMLTextAreaElement::Cols() const {
  return UnsignedOr(*this, u"cols", 20, /*zero_is_default=*/true);
}

void HTMLTextAreaElement::SetCols(uint32_t cols) {
  SetUnsignedOr(*this, u"cols", cols ? cols : 20, 20);
}

uint32_t HTMLTextAreaElement::Rows() const {
  return UnsignedOr(*this, u"rows", 2, /*zero_is_default=*/true);
}

void HTMLTextAreaElement::SetRows(uint32_t rows) {
  SetUnsignedOr(*this, u"rows", rows ? rows : 2, 2);
}

int32_t HTMLTextAreaElement::MaxLength() const {
  return TextControlMaxLength(*this);
}

void HTMLTextAreaElement::SetMaxLength(
    int32_t length,
    bindings::ExceptionState& exception_state) {
  SetTextControlMaxLength(*this, length, exception_state);
}

int32_t HTMLTextAreaElement::MinLength() const {
  return TextControlMinLength(*this);
}

void HTMLTextAreaElement::SetMinLength(
    int32_t length,
    bindings::ExceptionState& exception_state) {
  SetTextControlMinLength(*this, length, exception_state);
}

std::u16string HTMLTextAreaElement::IDLExposedAutofillValue() const {
  return ExposedAutofillValue(*this, /*is_hidden_input=*/false);
}

void HTMLTextAreaElement::SetIDLExposedAutofillValue(
    const std::u16string& value) {
  SetAttributeWithoutValidation(u"autocomplete", value);
}

// HTMLSelectElement.

std::u16string HTMLSelectElement::IDLExposedAutofillValue() const {
  return ExposedAutofillValue(*this, /*is_hidden_input=*/false);
}

void HTMLSelectElement::SetIDLExposedAutofillValue(
    const std::u16string& value) {
  SetAttributeWithoutValidation(u"autocomplete", value);
}

// HTMLOptionElement.

// static
HTMLOptionElement* HTMLOptionElement::CreateForJSConstructor(
    const std::u16string& data,
    const std::optional<std::u16string>& value,
    bool default_selected,
    bool selected) {
  Document& document = Window::Current().GetDocument();
  HTMLOptionElement* const option = Create(document);
  // A new option takes a text child without the checks that could throw.
  if (!data.empty()) {
    option->ParserAppendChild(document.CreateTextNode(data));
  }
  if (value) {
    option->SetValue(*value);
  }
  if (default_selected) {
    option->SetAttributeWithoutValidation(u"selected", u"");
  }
  return option;
}

// The text of its descendants but those of a script, HTML's or SVG's.
std::u16string HTMLOptionElement::CollapsedText() const {
  std::u16string text;
  const Node* node = FirstChild();
  while (node) {
    const Element* const element =
        node->IsElementNode() ? static_cast<const Element*>(node) : nullptr;
    const bool is_script =
        element && element->LocalName() == u"script" &&
        element->NamespaceURI() &&
        (*element->NamespaceURI() == html_names::kXhtmlNamespace ||
         *element->NamespaceURI() == html_names::kSvgNamespace);
    if (node->IsTextNode()) {
      text += static_cast<const CharacterData*>(node)->Data();
    }
    if (!is_script && node->FirstChild()) {
      node = node->FirstChild();
      continue;
    }
    while (node && node != this && !node->NextSibling()) {
      node = node->ParentNode();
    }
    node = node && node != this ? node->NextSibling() : nullptr;
  }
  std::u16string collapsed;
  bool space = false;
  for (const char16_t c : StripHTMLSpaces(text)) {
    if (IsHTMLSpace(c)) {
      space = true;
      continue;
    }
    if (space) {
      collapsed += u' ';
      space = false;
    }
    collapsed += c;
  }
  return collapsed;
}

std::u16string HTMLOptionElement::Label() const {
  const std::u16string* const label = FindAttribute(u"label");
  return label ? *label : CollapsedText();
}

void HTMLOptionElement::SetLabel(const std::u16string& label) {
  SetAttributeWithoutValidation(u"label", label);
}

std::u16string HTMLOptionElement::Value() const {
  const std::u16string* const value = FindAttribute(u"value");
  return value ? *value : CollapsedText();
}

void HTMLOptionElement::SetValue(const std::u16string& value) {
  SetAttributeWithoutValidation(u"value", value);
}

// HTMLMeterElement.

double HTMLMeterElement::Min() const {
  return NumberOr(*this, u"min", 0);
}

void HTMLMeterElement::SetMin(double min) {
  SetNumber(*this, u"min", min);
}

double HTMLMeterElement::Max() const {
  const double min = Min();
  return std::max(NumberOr(*this, u"max", std::max(1.0, min)), min);
}

void HTMLMeterElement::SetMax(double max) {
  SetNumber(*this, u"max", max);
}

double HTMLMeterElement::Value() const {
  return std::min(std::max(NumberOr(*this, u"value", 0), Min()), Max());
}

void HTMLMeterElement::SetValue(double value) {
  SetNumber(*this, u"value", value);
}

double HTMLMeterElement::Low() const {
  const double min = Min();
  return std::min(std::max(NumberOr(*this, u"low", min), min), Max());
}

void HTMLMeterElement::SetLow(double low) {
  SetNumber(*this, u"low", low);
}

double HTMLMeterElement::High() const {
  const double max = Max();
  return std::min(std::max(NumberOr(*this, u"high", max), Low()), max);
}

void HTMLMeterElement::SetHigh(double high) {
  SetNumber(*this, u"high", high);
}

double HTMLMeterElement::Optimum() const {
  const double min = Min();
  const double max = Max();
  return std::min(std::max(NumberOr(*this, u"optimum", (min + max) / 2), min),
                  max);
}

void HTMLMeterElement::SetOptimum(double optimum) {
  SetNumber(*this, u"optimum", optimum);
}

// HTMLProgressElement.

double HTMLProgressElement::Max() const {
  const double max = NumberOr(*this, u"max", 1);
  return max <= 0 ? 1 : max;
}

void HTMLProgressElement::SetMax(double max) {
  if (max > 0) {
    SetNumber(*this, u"max", max);
  }
}

double HTMLProgressElement::Value() const {
  const double value = NumberOr(*this, u"value", 0);
  return value < 0 ? 0 : std::min(value, Max());
}

void HTMLProgressElement::SetValue(double value) {
  SetNumber(*this, u"value", std::max(value, 0.0));
}

double HTMLProgressElement::Position() const {
  return FindAttribute(u"value") ? Value() / Max() : -1;
}

// HTMLTrackElement.

std::u16string HTMLTrackElement::Kind() const {
  const std::u16string* const kind = FindAttribute(u"kind");
  if (!kind) {
    return u"subtitles";
  }
  const std::u16string lower = base::ToLowerASCII(*kind);
  for (const std::u16string_view known :
       {u"subtitles", u"captions", u"descriptions", u"chapters", u"metadata"}) {
    if (lower == known) {
      return lower;
    }
  }
  return u"metadata";
}

void HTMLTrackElement::SetKind(const std::u16string& kind) {
  SetAttributeWithoutValidation(u"kind", kind);
}

// HTMLMarqueeElement.

uint32_t HTMLMarqueeElement::ScrollAmount() const {
  return UnsignedOr(*this, u"scrollamount", 6, /*zero_is_default=*/false);
}

void HTMLMarqueeElement::SetScrollAmount(uint32_t amount) {
  SetUnsignedOr(*this, u"scrollamount", amount, 6);
}

uint32_t HTMLMarqueeElement::ScrollDelay() const {
  return UnsignedOr(*this, u"scrolldelay", 85, /*zero_is_default=*/false);
}

void HTMLMarqueeElement::SetScrollDelay(uint32_t delay) {
  SetUnsignedOr(*this, u"scrolldelay", delay, 85);
}

// HTMLEmbedElement, HTMLObjectElement, HTMLIFrameElement and
// HTMLSourceElement: a Trusted Type would be checked here, which no policy
// asks for.

void HTMLEmbedElement::SetSrc(const std::u16string& src,
                              bindings::ExceptionState& exception_state) {
  SetAttributeWithoutValidation(u"src", src);
}

void HTMLObjectElement::SetData(const std::u16string& data,
                                bindings::ExceptionState& exception_state) {
  SetAttributeWithoutValidation(u"data", data);
}

void HTMLObjectElement::SetCodeBase(const std::u16string& code_base,
                                    bindings::ExceptionState& exception_state) {
  SetAttributeWithoutValidation(u"codebase", code_base);
}

std::u16string HTMLIFrameElement::Srcdoc() const {
  const std::u16string* const srcdoc = FindAttribute(u"srcdoc");
  return srcdoc ? *srcdoc : u"";
}

void HTMLIFrameElement::SetSrcdoc(const std::u16string& srcdoc,
                                  bindings::ExceptionState& exception_state) {
  SetAttributeWithoutValidation(u"srcdoc", srcdoc);
}

std::u16string HTMLSourceElement::Type() const {
  const std::u16string* const type = FindAttribute(u"type");
  return type ? *type : u"";
}

void HTMLSourceElement::SetType(const std::u16string& type) {
  SetAttributeWithoutValidation(u"type", type);
}

namespace {

// The source's parent, if it is a picture element.
HTMLPictureElement* PictureParent(Node* parent) {
  if (!parent || !parent->IsElementNode() ||
      !static_cast<Element*>(parent)->HasHTMLTagName(u"picture")) {
    return nullptr;
  }
  return static_cast<HTMLPictureElement*>(parent);
}

}  // namespace

// Upstream: third_party/blink/renderer/core/html/html_source_element.cc
bool HTMLSourceElement::MediaQueryMatches() const {
  const std::u16string* const media = FindAttribute(u"media");
  return !media || MediaAttributeMatches(GetDocument(), *media);
}

Node::InsertionNotificationRequest HTMLSourceElement::InsertedInto(
    Node& insertion_point) {
  HTMLElement::InsertedInto(insertion_point);
  if (ParentNode() == &insertion_point) {
    if (HTMLPictureElement* const picture = PictureParent(ParentNode())) {
      picture->SourceChanged();
    }
  }
  return InsertionNotificationRequest::kInsertionDone;
}

void HTMLSourceElement::RemovedFrom(Node& insertion_point) {
  if (!ParentNode()) {
    if (HTMLPictureElement* const picture = PictureParent(&insertion_point)) {
      picture->SourceChanged();
    }
  }
  HTMLElement::RemovedFrom(insertion_point);
}

void HTMLSourceElement::AttributeChanged(const QualifiedName& name,
                                         const std::u16string* old_value,
                                         const std::u16string* new_value) {
  HTMLElement::AttributeChanged(name, old_value, new_value);
  if (!name.namespace_uri &&
      (name.local_name == u"srcset" || name.local_name == u"sizes" ||
       name.local_name == u"media" || name.local_name == u"type")) {
    if (HTMLPictureElement* const picture = PictureParent(ParentNode())) {
      picture->SourceChanged();
    }
  }
}

}  // namespace mitto
