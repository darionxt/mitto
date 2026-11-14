#include "mitto/dom/html_element_factory.h"

#include <algorithm>
#include <array>
#include <string_view>

#include "base/strings/string_util.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/bindings/runtime_features.h"
#include "mitto/dom/html_anchor_element.h"
#include "mitto/dom/html_area_element.h"
#include "mitto/dom/html_audio_element.h"
#include "mitto/dom/html_base_element.h"
#include "mitto/dom/html_body_element.h"
#include "mitto/dom/html_br_element.h"
#include "mitto/dom/html_canvas_element.h"
#include "mitto/dom/html_details_element.h"
#include "mitto/dom/html_dialog_element.h"
#include "mitto/dom/html_div_element.h"
#include "mitto/dom/html_element.h"
#include "mitto/dom/html_elements.h"
#include "mitto/dom/html_head_element.h"
#include "mitto/dom/html_heading_element.h"
#include "mitto/dom/html_html_element.h"
#include "mitto/dom/html_image_element.h"
#include "mitto/dom/html_input_element.h"
#include "mitto/dom/html_li_element.h"
#include "mitto/dom/html_link_element.h"
#include "mitto/dom/html_meta_element.h"
#include "mitto/dom/html_olist_element.h"
#include "mitto/dom/html_paragraph_element.h"
#include "mitto/dom/html_pre_element.h"
#include "mitto/dom/html_script_element.h"
#include "mitto/dom/html_span_element.h"
#include "mitto/dom/html_style_element.h"
#include "mitto/dom/html_table_elements.h"
#include "mitto/dom/html_template_element.h"
#include "mitto/dom/html_title_element.h"
#include "mitto/dom/html_ulist_element.h"
#include "mitto/dom/html_unknown_element.h"
#include "mitto/dom/qualified_name.h"
#include "v8/include/v8-isolate.h"

// Upstream: third_party/blink/renderer/core/html/html_tag_names.json5
// Upstream: third_party/blink/renderer/core/html/custom/custom_element.cc

namespace mitto {

namespace {

// The names that Blink's html_tag_names.json5 lists, other than those whose
// element is an HTMLUnknownElement and those behind a feature that is off for
// stable, sorted.
constexpr auto kKnownNames = std::to_array<std::u16string_view>({
    u"a",          u"abbr",
    u"acronym",    u"address",
    u"area",       u"article",
    u"aside",      u"audio",
    u"b",          u"base",
    u"basefont",   u"bdi",
    u"bdo",        u"big",
    u"blockquote", u"body",
    u"br",         u"button",
    u"canvas",     u"caption",
    u"center",     u"cite",
    u"code",       u"col",
    u"colgroup",   u"data",
    u"datalist",   u"dd",
    u"del",        u"details",
    u"dfn",        u"dialog",
    u"dir",        u"div",
    u"dl",         u"dt",
    u"em",         u"embed",
    u"fieldset",   u"figcaption",
    u"figure",     u"font",
    u"footer",     u"form",
    u"frame",      u"frameset",
    u"h1",         u"h2",
    u"h3",         u"h4",
    u"h5",         u"h6",
    u"head",       u"header",
    u"hgroup",     u"hr",
    u"html",       u"i",
    u"iframe",     u"img",
    u"input",      u"ins",
    u"kbd",        u"label",
    u"layer",      u"legend",
    u"li",         u"link",
    u"listing",    u"main",
    u"map",        u"mark",
    u"marquee",    u"menu",
    u"meta",       u"meter",
    u"nav",        u"nobr",
    u"noembed",    u"noframes",
    u"nolayer",    u"noscript",
    u"object",     u"ol",
    u"optgroup",   u"option",
    u"output",     u"p",
    u"param",      u"picture",
    u"plaintext",  u"pre",
    u"progress",   u"q",
    u"rb",         u"rp",
    u"rt",         u"rtc",
    u"ruby",       u"s",
    u"samp",       u"script",
    u"search",     u"section",
    u"select",     u"selectedcontent",
    u"slot",       u"small",
    u"source",     u"span",
    u"strike",     u"strong",
    u"style",      u"sub",
    u"summary",    u"sup",
    u"table",      u"tbody",
    u"td",         u"template",
    u"textarea",   u"tfoot",
    u"th",         u"thead",
    u"time",       u"title",
    u"tr",         u"track",
    u"tt",         u"u",
    u"ul",         u"var",
    u"video",      u"wbr",
    u"xmp",
});

using Maker = HTMLElement* (*)(Document& document,
                               const std::u16string& local_name);

template <typename T>
HTMLElement* Make(Document& document, const std::u16string& local_name) {
  return T::Create(document);
}

// For a class of several names.
template <typename T>
HTMLElement* MakeNamed(Document& document, const std::u16string& local_name) {
  return T::Create(document, local_name);
}

struct NamedMaker {
  std::u16string_view name;
  Maker make;
};

// The names whose interface has a class in the Runtime, sorted, as Blink's
// generated HTMLElementFactory maps them.
constexpr auto kElements = std::to_array<NamedMaker>({
    {u"a", &Make<HTMLAnchorElement>},
    {u"area", &Make<HTMLAreaElement>},
    {u"audio", &Make<HTMLAudioElement>},
    {u"base", &Make<HTMLBaseElement>},
    {u"blockquote", &MakeNamed<HTMLQuoteElement>},
    {u"body", &Make<HTMLBodyElement>},
    {u"br", &Make<HTMLBRElement>},
    {u"button", &Make<HTMLButtonElement>},
    {u"canvas", &Make<HTMLCanvasElement>},
    {u"caption", &Make<HTMLTableCaptionElement>},
    {u"col", &MakeNamed<HTMLTableColElement>},
    {u"colgroup", &MakeNamed<HTMLTableColElement>},
    {u"data", &Make<HTMLDataElement>},
    {u"datalist", &Make<HTMLDataListElement>},
    {u"del", &MakeNamed<HTMLModElement>},
    {u"details", &Make<HTMLDetailsElement>},
    {u"dialog", &Make<HTMLDialogElement>},
    {u"dir", &Make<HTMLDirectoryElement>},
    {u"div", &Make<HTMLDivElement>},
    {u"dl", &Make<HTMLDListElement>},
    {u"embed", &Make<HTMLEmbedElement>},
    {u"fieldset", &Make<HTMLFieldSetElement>},
    {u"font", &Make<HTMLFontElement>},
    {u"form", &Make<HTMLFormElement>},
    {u"frame", &Make<HTMLFrameElement>},
    {u"frameset", &Make<HTMLFrameSetElement>},
    {u"h1", &MakeNamed<HTMLHeadingElement>},
    {u"h2", &MakeNamed<HTMLHeadingElement>},
    {u"h3", &MakeNamed<HTMLHeadingElement>},
    {u"h4", &MakeNamed<HTMLHeadingElement>},
    {u"h5", &MakeNamed<HTMLHeadingElement>},
    {u"h6", &MakeNamed<HTMLHeadingElement>},
    {u"head", &Make<HTMLHeadElement>},
    {u"hr", &Make<HTMLHRElement>},
    {u"html", &Make<HTMLHtmlElement>},
    {u"iframe", &Make<HTMLIFrameElement>},
    {u"img", &Make<HTMLImageElement>},
    {u"input", &Make<HTMLInputElement>},
    {u"ins", &MakeNamed<HTMLModElement>},
    {u"label", &Make<HTMLLabelElement>},
    {u"legend", &Make<HTMLLegendElement>},
    {u"li", &Make<HTMLLIElement>},
    {u"link", &Make<HTMLLinkElement>},
    {u"listing", &MakeNamed<HTMLPreElement>},
    {u"map", &Make<HTMLMapElement>},
    {u"marquee", &Make<HTMLMarqueeElement>},
    {u"menu", &Make<HTMLMenuElement>},
    {u"meta", &Make<HTMLMetaElement>},
    {u"meter", &Make<HTMLMeterElement>},
    {u"object", &Make<HTMLObjectElement>},
    {u"ol", &Make<HTMLOListElement>},
    {u"optgroup", &Make<HTMLOptGroupElement>},
    {u"option", &Make<HTMLOptionElement>},
    {u"output", &Make<HTMLOutputElement>},
    {u"p", &Make<HTMLParagraphElement>},
    {u"param", &Make<HTMLParamElement>},
    {u"picture", &Make<HTMLPictureElement>},
    {u"pre", &MakeNamed<HTMLPreElement>},
    {u"progress", &Make<HTMLProgressElement>},
    {u"q", &MakeNamed<HTMLQuoteElement>},
    {u"script", &Make<HTMLScriptElement>},
    {u"select", &Make<HTMLSelectElement>},
    {u"selectedcontent", &Make<HTMLSelectedContentElement>},
    {u"slot", &Make<HTMLSlotElement>},
    {u"source", &Make<HTMLSourceElement>},
    {u"span", &Make<HTMLSpanElement>},
    {u"style", &Make<HTMLStyleElement>},
    {u"table", &Make<HTMLTableElement>},
    {u"tbody", &MakeNamed<HTMLTableSectionElement>},
    {u"td", &MakeNamed<HTMLTableCellElement>},
    {u"template", &Make<HTMLTemplateElement>},
    {u"textarea", &Make<HTMLTextAreaElement>},
    {u"tfoot", &MakeNamed<HTMLTableSectionElement>},
    {u"th", &MakeNamed<HTMLTableCellElement>},
    {u"thead", &MakeNamed<HTMLTableSectionElement>},
    {u"time", &Make<HTMLTimeElement>},
    {u"title", &Make<HTMLTitleElement>},
    {u"tr", &Make<HTMLTableRowElement>},
    {u"track", &Make<HTMLTrackElement>},
    {u"ul", &Make<HTMLUListElement>},
    {u"video", &Make<HTMLVideoElement>},
    {u"xmp", &MakeNamed<HTMLPreElement>},
});

struct GatedMaker {
  std::u16string_view name;
  bindings::RuntimeFeature feature;
  Maker make;
};

// The names whose element is behind a runtime-enabled feature, sorted, which
// Blink's factory makes only while the feature is on, and otherwise leaves
// to HTMLUnknownElement.
constexpr auto kGatedElements = std::to_array<GatedMaker>({
    {u"camera", bindings::RuntimeFeature::kCameraAndMicrophoneElements,
     &Make<HTMLCameraElement>},
    {u"credential", bindings::RuntimeFeature::kLoginElement,
     &Make<HTMLCredentialElement>},
    {u"fencedframe", bindings::RuntimeFeature::kFencedFrames,
     &Make<HTMLFencedFrameElement>},
    {u"geolocation", bindings::RuntimeFeature::kGeolocationElement,
     &Make<HTMLGeolocationElement>},
    {u"install", bindings::RuntimeFeature::kInstallElement,
     &Make<HTMLInstallElement>},
    {u"login", bindings::RuntimeFeature::kLoginElement,
     &Make<HTMLLoginElement>},
    {u"menubar", bindings::RuntimeFeature::kMenuElements,
     &Make<HTMLMenuBarElement>},
    {u"menuitem", bindings::RuntimeFeature::kMenuElements,
     &Make<HTMLMenuItemElement>},
    {u"menulist", bindings::RuntimeFeature::kMenuElements,
     &Make<HTMLMenuListElement>},
    {u"microphone", bindings::RuntimeFeature::kCameraAndMicrophoneElements,
     &Make<HTMLMicrophoneElement>},
    {u"persistentwidget", bindings::RuntimeFeature::kPersistentWidgetAPI,
     &Make<HTMLPersistentWidgetElement>},
    {u"submenu", bindings::RuntimeFeature::kMenuElements,
     &Make<HTMLSubMenuElement>},
    {u"usermedia", bindings::RuntimeFeature::kUserMediaElement,
     &Make<HTMLUserMediaElement>},
});

bool IsFeatureEnabled(bindings::RuntimeFeature feature) {
  v8::Isolate* const isolate = v8::Isolate::TryGetCurrent();
  const bindings::RuntimeFeatureSet* const features =
      isolate ? bindings::PerIsolateData::FeaturesOf(isolate) : nullptr;
  return features && features->IsEnabled(feature);
}

// The HTML Standard's names that a valid custom element name cannot be.
constexpr auto kReservedCustomElementNames = std::to_array<std::u16string_view>(
    {u"annotation-xml", u"color-profile", u"font-face", u"font-face-format",
     u"font-face-name", u"font-face-src", u"font-face-uri", u"missing-glyph"});

}  // namespace

bool IsValidCustomElementName(std::u16string_view name) {
  // A hyphen after a lower-case ASCII letter, in a valid element local name
  // with no upper-case ASCII letter, which no element of the HTML Standard,
  // SVG or MathML has.
  if (name.size() < 2 || name.find(u'-', 1) == std::u16string_view::npos ||
      !base::IsAsciiLower(name[0]) || !IsValidElementLocalName(name) ||
      std::any_of(name.begin(), name.end(),
                  [](char16_t c) { return base::IsAsciiUpper(c); })) {
    return false;
  }
  return std::find(kReservedCustomElementNames.begin(),
                   kReservedCustomElementNames.end(),
                   name) == kReservedCustomElementNames.end();
}

bool IsKnownHTMLElementName(const std::u16string& local_name) {
  return std::binary_search(kKnownNames.begin(), kKnownNames.end(),
                            std::u16string_view(local_name));
}

HTMLElement* CreateHTMLElement(Document& document,
                               const std::u16string& local_name) {
  const std::u16string_view name = local_name;
  const auto found =
      std::lower_bound(kElements.begin(), kElements.end(), name,
                       [](const NamedMaker& entry, std::u16string_view key) {
                         return entry.name < key;
                       });
  if (found != kElements.end() && found->name == name) {
    return found->make(document, local_name);
  }
  for (const GatedMaker& gated : kGatedElements) {
    if (gated.name == name && IsFeatureEnabled(gated.feature)) {
      return gated.make(document, local_name);
    }
  }
  if (IsKnownHTMLElementName(local_name) ||
      IsValidCustomElementName(local_name)) {
    return HTMLElement::Create(document, local_name);
  }
  return HTMLUnknownElement::Create(document, local_name);
}

}  // namespace mitto
