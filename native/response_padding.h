// Chrome's padding of an opaque response, which the Cache API counts beside
// its size, so that the usage that a page reads tells nothing of another
// origin's response.
//
// Upstream: storage/common/quota/padding_key.h

#ifndef MITTO_RESPONSE_PADDING_H_
#define MITTO_RESPONSE_PADDING_H_

#include <stdint.h>

#include <string_view>

#include "base/time/time.h"
#include "url/origin.h"

namespace mitto {

// The padding of a response that came from the network, below
// kResponsePaddingRange.
int64_t ComputeRandomResponsePadding();

// The padding of a response that the HTTP cache gave to a context of `origin`,
// the same for each of its hits, so that no page averages it away.
int64_t ComputeStableResponsePadding(const url::Origin& origin,
                                     std::string_view response_url,
                                     base::Time response_time,
                                     std::string_view request_method);

}  // namespace mitto

#endif  // MITTO_RESPONSE_PADDING_H_
