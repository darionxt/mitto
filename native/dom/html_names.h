// The namespaces of the HTML Standard's parser and the DOM's elements and
// attributes.
//
// Upstream: third_party/blink/renderer/core/html/html_element.h
// Upstream: third_party/blink/renderer/core/svg/svg_tag_names.json5
// Upstream: third_party/blink/renderer/core/mathml/mathml_tag_names.json5
// Upstream: third_party/blink/renderer/core/xml/xml_attribute_names.json5
// Upstream: third_party/blink/renderer/core/xml/xmlns_attribute_names.json5
// Upstream: third_party/blink/renderer/core/svg/xlink_attribute_names.json5

#ifndef MITTO_DOM_HTML_NAMES_H_
#define MITTO_DOM_HTML_NAMES_H_

namespace mitto::html_names {

inline constexpr char16_t kXhtmlNamespace[] = u"http://www.w3.org/1999/xhtml";
inline constexpr char16_t kSvgNamespace[] = u"http://www.w3.org/2000/svg";
inline constexpr char16_t kMathMlNamespace[] =
    u"http://www.w3.org/1998/Math/MathML";
inline constexpr char16_t kXmlNamespace[] =
    u"http://www.w3.org/XML/1998/namespace";
inline constexpr char16_t kXmlnsNamespace[] = u"http://www.w3.org/2000/xmlns/";
inline constexpr char16_t kXlinkNamespace[] = u"http://www.w3.org/1999/xlink";

}  // namespace mitto::html_names

#endif  // MITTO_DOM_HTML_NAMES_H_
