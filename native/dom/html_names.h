// The namespaces of the HTML Standard's parser and the DOM's elements.
//
// Upstream: third_party/blink/renderer/core/html/html_element.h

#ifndef MITTO_DOM_HTML_NAMES_H_
#define MITTO_DOM_HTML_NAMES_H_

namespace mitto::html_names {

inline constexpr char16_t kXhtmlNamespace[] = u"http://www.w3.org/1999/xhtml";
inline constexpr char16_t kSvgNamespace[] = u"http://www.w3.org/2000/svg";
inline constexpr char16_t kMathMlNamespace[] =
    u"http://www.w3.org/1998/Math/MathML";

}  // namespace mitto::html_names

#endif  // MITTO_DOM_HTML_NAMES_H_
