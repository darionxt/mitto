#include "mitto/dom/html_element_factory.h"

#include <algorithm>
#include <array>
#include <string_view>

#include "base/strings/string_util.h"
#include "mitto/dom/html_body_element.h"
#include "mitto/dom/html_br_element.h"
#include "mitto/dom/html_div_element.h"
#include "mitto/dom/html_element.h"
#include "mitto/dom/html_head_element.h"
#include "mitto/dom/html_heading_element.h"
#include "mitto/dom/html_html_element.h"
#include "mitto/dom/html_li_element.h"
#include "mitto/dom/html_link_element.h"
#include "mitto/dom/html_meta_element.h"
#include "mitto/dom/html_olist_element.h"
#include "mitto/dom/html_paragraph_element.h"
#include "mitto/dom/html_pre_element.h"
#include "mitto/dom/html_script_element.h"
#include "mitto/dom/html_span_element.h"
#include "mitto/dom/html_style_element.h"
#include "mitto/dom/html_template_element.h"
#include "mitto/dom/html_title_element.h"
#include "mitto/dom/html_ulist_element.h"
#include "mitto/dom/html_unknown_element.h"
#include "mitto/dom/qualified_name.h"

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
  if (name == u"html") {
    return HTMLHtmlElement::Create(document);
  }
  if (name == u"head") {
    return HTMLHeadElement::Create(document);
  }
  if (name == u"title") {
    return HTMLTitleElement::Create(document);
  }
  if (name == u"meta") {
    return HTMLMetaElement::Create(document);
  }
  if (name == u"link") {
    return HTMLLinkElement::Create(document);
  }
  if (name == u"style") {
    return HTMLStyleElement::Create(document);
  }
  if (name == u"script") {
    return HTMLScriptElement::Create(document);
  }
  if (name == u"body") {
    return HTMLBodyElement::Create(document);
  }
  if (name == u"div") {
    return HTMLDivElement::Create(document);
  }
  if (name == u"span") {
    return HTMLSpanElement::Create(document);
  }
  if (name == u"p") {
    return HTMLParagraphElement::Create(document);
  }
  if (name == u"br") {
    return HTMLBRElement::Create(document);
  }
  if (name == u"ul") {
    return HTMLUListElement::Create(document);
  }
  if (name == u"ol") {
    return HTMLOListElement::Create(document);
  }
  if (name == u"li") {
    return HTMLLIElement::Create(document);
  }
  if (name == u"template") {
    return HTMLTemplateElement::Create(document);
  }
  if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') {
    return HTMLHeadingElement::Create(document, local_name);
  }
  if (name == u"pre" || name == u"listing" || name == u"xmp") {
    return HTMLPreElement::Create(document, local_name);
  }
  if (IsKnownHTMLElementName(local_name) ||
      IsValidCustomElementName(local_name)) {
    return HTMLElement::Create(document, local_name);
  }
  return HTMLUnknownElement::Create(document, local_name);
}

}  // namespace mitto
