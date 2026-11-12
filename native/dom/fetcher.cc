#include "mitto/dom/fetcher.h"

namespace mitto {

FetchRequestData::FetchRequestData() = default;
FetchRequestData::FetchRequestData(FetchRequestData&&) = default;
FetchRequestData& FetchRequestData::operator=(FetchRequestData&&) = default;
FetchRequestData::~FetchRequestData() = default;

FetchResponseHead::FetchResponseHead() = default;
FetchResponseHead::FetchResponseHead(const FetchResponseHead&) = default;
FetchResponseHead& FetchResponseHead::operator=(const FetchResponseHead&) =
    default;
FetchResponseHead::~FetchResponseHead() = default;

}  // namespace mitto
