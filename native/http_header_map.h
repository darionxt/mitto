// The order in which Blink hands a subresource's headers to the network
// service.
//
// Blink holds them in an HTTPHeaderMap, which is a WTF::HashMap, and goes
// through it from the first bucket to the last. So where a header goes depends
// on its name, and on how many others there are, because the table grows with
// them. The source export leaves Blink out, so this is ported from
// third_party/blink/renderer/platform/network/http_header_map.h and
// .../wtf/hash_table.h, with the hash of a name in string_hasher.h.
//
// Upstream: third_party/blink/renderer/platform/network/http_header_map.h
// Upstream: third_party/blink/renderer/platform/wtf/hash_table.h

#ifndef MITTO_HTTP_HEADER_MAP_H_
#define MITTO_HTTP_HEADER_MAP_H_

#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"

namespace mitto {

// Returns `names`, which are valid header names and differ even in another
// case, in the order that a map gives them after they were added in this order.
// That order only counts where two names land in the same bucket.
std::vector<std::string> HTTPHeaderMapOrder(
    base::span<const std::string_view> names);

}  // namespace mitto

#endif  // MITTO_HTTP_HEADER_MAP_H_
