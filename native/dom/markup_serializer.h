// The markup of a node: the HTML Standard's HTML fragment serialization
// algorithm, which innerHTML and outerHTML read in an HTML document, and the
// DOM Parsing specification's XML serialization, which they read in any other
// document and XMLSerializer reads everywhere, with what Blink escapes and
// how it picks namespace prefixes.
//
// Upstream: third_party/blink/renderer/core/editing/serializers/markup_accumulator.cc
// Upstream: third_party/blink/renderer/core/editing/serializers/markup_formatter.cc
// Upstream: third_party/blink/renderer/core/editing/serializers/serialization.cc

#ifndef MITTO_DOM_MARKUP_SERIALIZER_H_
#define MITTO_DOM_MARKUP_SERIALIZER_H_

#include <string>
#include <string_view>

namespace mitto {

class Node;

// The markup of `node`'s children, as HTML in an HTML document and as XML in
// any other, as Blink's CreateMarkup() picks.
std::u16string SerializeChildren(const Node& node);

// The markup of `node` and its children, picked the same way.
std::u16string SerializeNode(const Node& node);

// The markup of `node` and its children as XML, whatever its document, as
// XMLSerializer's serializeToString() writes it. An attribute's node gives
// its value.
std::u16string SerializeNodeAsXml(const Node& node);

// An attribute's value as Blink's MarkupFormatter::AppendAttributeValue()
// escapes it: for HTML, or else for XML, which writes tabs and line breaks as
// character references and leaves a no-break space as it is.
std::u16string EscapeAttributeValue(std::u16string_view value, bool for_html);

}  // namespace mitto

#endif  // MITTO_DOM_MARKUP_SERIALIZER_H_
