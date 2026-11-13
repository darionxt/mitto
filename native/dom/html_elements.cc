#include "mitto/dom/html_elements.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_button_element.h"
#include "mitto/bindings/v8_html_camera_element.h"
#include "mitto/bindings/v8_html_credential_element.h"
#include "mitto/bindings/v8_html_data_element.h"
#include "mitto/bindings/v8_html_data_list_element.h"
#include "mitto/bindings/v8_html_details_element.h"
#include "mitto/bindings/v8_html_dialog_element.h"
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
#include "mitto/bindings/v8_html_table_cell_element.h"
#include "mitto/bindings/v8_html_table_col_element.h"
#include "mitto/bindings/v8_html_table_element.h"
#include "mitto/bindings/v8_html_table_row_element.h"
#include "mitto/bindings/v8_html_table_section_element.h"
#include "mitto/bindings/v8_html_text_area_element.h"
#include "mitto/bindings/v8_html_time_element.h"
#include "mitto/bindings/v8_html_track_element.h"
#include "mitto/bindings/v8_html_user_media_element.h"
#include "mitto/bindings/v8_html_video_element.h"

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
HTMLDetailsElement* HTMLDetailsElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLDetailsElement>(document);
}

HTMLDetailsElement::HTMLDetailsElement(Document& document)
    : HTMLElement(document, u"details") {}

const bindings::WrapperTypeInfo* HTMLDetailsElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLDetailsElement::kWrapperTypeInfo;
}

// static
HTMLDialogElement* HTMLDialogElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLDialogElement>(document);
}

HTMLDialogElement::HTMLDialogElement(Document& document)
    : HTMLElement(document, u"dialog") {}

const bindings::WrapperTypeInfo* HTMLDialogElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLDialogElement::kWrapperTypeInfo;
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

// static
HTMLFrameElement* HTMLFrameElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLFrameElement>(document);
}

HTMLFrameElement::HTMLFrameElement(Document& document)
    : HTMLElement(document, u"frame") {}

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
    : HTMLElement(document, u"iframe") {}

const bindings::WrapperTypeInfo* HTMLIFrameElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLIFrameElement::kWrapperTypeInfo;
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
HTMLTableCellElement* HTMLTableCellElement::Create(
    Document& document,
    const std::u16string& local_name) {
  return bindings::MakeGarbageCollected<HTMLTableCellElement>(document,
                                                              local_name);
}

HTMLTableCellElement::HTMLTableCellElement(Document& document,
                                           const std::u16string& local_name)
    : HTMLElement(document, local_name) {}

const bindings::WrapperTypeInfo* HTMLTableCellElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLTableCellElement::kWrapperTypeInfo;
}

// static
HTMLTableColElement* HTMLTableColElement::Create(
    Document& document,
    const std::u16string& local_name) {
  return bindings::MakeGarbageCollected<HTMLTableColElement>(document,
                                                             local_name);
}

HTMLTableColElement::HTMLTableColElement(Document& document,
                                         const std::u16string& local_name)
    : HTMLElement(document, local_name) {}

const bindings::WrapperTypeInfo* HTMLTableColElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLTableColElement::kWrapperTypeInfo;
}

// static
HTMLTableElement* HTMLTableElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLTableElement>(document);
}

HTMLTableElement::HTMLTableElement(Document& document)
    : HTMLElement(document, u"table") {}

const bindings::WrapperTypeInfo* HTMLTableElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLTableElement::kWrapperTypeInfo;
}

// static
HTMLTableRowElement* HTMLTableRowElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLTableRowElement>(document);
}

HTMLTableRowElement::HTMLTableRowElement(Document& document)
    : HTMLElement(document, u"tr") {}

const bindings::WrapperTypeInfo* HTMLTableRowElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLTableRowElement::kWrapperTypeInfo;
}

// static
HTMLTableSectionElement* HTMLTableSectionElement::Create(
    Document& document,
    const std::u16string& local_name) {
  return bindings::MakeGarbageCollected<HTMLTableSectionElement>(document,
                                                                 local_name);
}

HTMLTableSectionElement::HTMLTableSectionElement(
    Document& document,
    const std::u16string& local_name)
    : HTMLElement(document, local_name) {}

const bindings::WrapperTypeInfo* HTMLTableSectionElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLTableSectionElement::kWrapperTypeInfo;
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

}  // namespace mitto
