// What the Page's loading tells the Performance Timeline of a response: the
// ResourceTimingInfo of a request, as Blink's CreateResourceTimingInfo() makes
// it from the response that Chrome's network service gave, and the header
// helpers it shares with the document's loading.
//
// Upstream: third_party/blink/renderer/platform/loader/fetch/resource_timing_utils.cc
// Upstream: third_party/blink/renderer/platform/loader/fetch/resource_response.cc
// Upstream: third_party/blink/renderer/platform/network/http_parsers.cc
// Upstream: third_party/blink/common/mime_util/mime_util.cc
// Upstream: media/base/mime_util_internal.cc
// Upstream: net/base/mime_util.cc

#ifndef MITTO_PAGE_RESOURCE_TIMING_H_
#define MITTO_PAGE_RESOURCE_TIMING_H_

#include <stdint.h>

#include <string>
#include <string_view>

#include "base/time/time.h"
#include "mitto/dom/document_load_timing.h"
#include "mitto/page_messages.h"

namespace mitto {

// The values of each header line named `name`, joined by commas, as Blink's
// ResourceResponse::HttpHeaderField() joins a header that comes more than
// once. `headers` holds a line feed after each "name: value".
std::string HeaderField(std::string_view headers, std::string_view name);

// Blink's ResourceResponse::HttpContentType(): the MIME type of the last
// media type in Content-Type that has one, in lower case, without parameters.
std::string HttpContentType(std::string_view headers);

// The MIME Sniffing Standard's minimized supported MIME type of `mime_type`,
// as Blink's MinimizedMIMEType() works it out, or empty.
std::string MinimizedMimeType(std::string_view mime_type);

// ResourceResponse::GetFilteredHttpContentEncoding(): Content-Encoding when it
// is one coding that Chrome knows, "multiple" for a list, "@unknown" for any
// other, and empty without one.
std::string FilteredContentEncoding(std::string_view headers);

// base::TimeTicks of a time that a message carries, null for 0.
base::TimeTicks TicksOf(int64_t microseconds);

// CreateResourceTimingInfo() for the request that started at `start_time` at
// `initial_url`, with `response`, or none for one that failed before it had
// one, which ended at `response_end`.
ResourceTimingInfo CreateResourceTimingInfo(base::TimeTicks start_time,
                                            std::u16string initial_url,
                                            const PageResponseStarted* response,
                                            base::TimeTicks response_end);

// What the end of a request adds: the sizes of its body, and the cache's
// state, which Blink takes from the bytes it received in all.
void SetResourceTimingSizes(ResourceTimingInfo& info,
                            const PageResponseStarted& response,
                            int64_t encoded_body_size,
                            int64_t received_bytes,
                            uint64_t decoded_body_size);

}  // namespace mitto

#endif  // MITTO_PAGE_RESOURCE_TIMING_H_
