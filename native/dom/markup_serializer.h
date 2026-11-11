// The HTML Standard's HTML fragment serialization algorithm, which innerHTML
// and outerHTML read, with what Blink escapes.
//
// Upstream: third_party/blink/renderer/core/editing/serializers/markup_accumulator.cc
// Upstream: third_party/blink/renderer/core/editing/serializers/markup_formatter.cc

#ifndef MITTO_DOM_MARKUP_SERIALIZER_H_
#define MITTO_DOM_MARKUP_SERIALIZER_H_

#include <string>

namespace mitto {

class Node;

// The markup of `node`'s children.
std::u16string SerializeChildren(const Node& node);

// The markup of `node` and its children.
std::u16string SerializeNode(const Node& node);

}  // namespace mitto

#endif  // MITTO_DOM_MARKUP_SERIALIZER_H_
