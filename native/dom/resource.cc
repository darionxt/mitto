#include "mitto/dom/resource.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "base/check.h"
#include "mitto/dom/resource_fetcher.h"

namespace mitto {
namespace {

// What a request's network error would be named if the Page program, with no
// //net, could name it: a response of an error status that a resource does
// not ignore fails as ResourceError::HttpError(),
// net::ERR_HTTP_RESPONSE_CODE_FAILURE.
constexpr int kHttpResponseCodeFailure = -379;

// Resource::ShouldIgnoreHTTPStatusCodeErrors(): an image takes any status.
bool IgnoresHttpStatusErrors(ResourceType type) {
  // ImageResource's and RawResource's ShouldIgnoreHTTPStatusCodeErrors().
  return type == ResourceType::kImage || type == ResourceType::kRaw;
}

bool HasMetadata(const std::vector<IntegrityMetadata>& set,
                 const IntegrityMetadata& metadata) {
  return std::ranges::any_of(set, [&metadata](const IntegrityMetadata& item) {
    return item.algorithm == metadata.algorithm && item.value == metadata.value;
  });
}

bool SameMetadata(const std::vector<IntegrityMetadata>& a,
                  const std::vector<IntegrityMetadata>& b) {
  return std::ranges::all_of(a,
                             [&b](const IntegrityMetadata& item) {
                               return HasMetadata(b, item);
                             }) &&
         std::ranges::all_of(b, [&a](const IntegrityMetadata& item) {
           return HasMetadata(a, item);
         });
}

}  // namespace

bool IntegritySetsEqual(const IntegrityMetadataSet& a,
                        const IntegrityMetadataSet& b) {
  return SameMetadata(a.hashes, b.hashes) &&
         SameMetadata(a.public_keys, b.public_keys);
}

Resource::Resource(ResourceType type,
                   GURL url,
                   FetchMode mode,
                   FetchCredentialsMode credentials)
    : type_(type),
      url_(std::move(url)),
      mode_(mode),
      credentials_(credentials) {}

Resource::~Resource() = default;

void Resource::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(clients_);
  visitor->Trace(fetcher_);
}

void Resource::AddClient(ResourceClient* client) {
  clients_.push_back(client);
  // A client of a resource that has ended hears of it at once, as Blink's
  // ImageResourceContent::AddObserver() tells it.
  if (IsLoaded()) {
    client->NotifyFinished(*this);
  }
}

void Resource::RemoveClient(ResourceClient* client) {
  // One of a client added more than once, as Blink's counted set of clients
  // keeps one for each add.
  bool removed = false;
  clients_.EraseIf([client, &removed](ResourceClient* item) {
    if (removed || item != client) {
      return false;
    }
    removed = true;
    return true;
  });
  if (removed && !HasClients() && IsLoading() && cancel_load_) {
    if (ResourceFetcher* const fetcher = fetcher_.Get()) {
      fetcher->ScheduleCancel(*this);
    }
  }
}

bool Resource::HasClients() const {
  return std::ranges::any_of(clients_.ToVector(),
                             [](ResourceClient* client) { return client; });
}

void Resource::Block() {
  blocked_ = true;
  Finish(kBlockedByClient);
}

void Resource::ResponseReceived(FetchResponseHead head, std::string mime_type) {
  response_ = std::move(head);
  mime_type_ = std::move(mime_type);
  for (ResourceClient* client : clients_.ToVector()) {
    if (client) {
      client->ResponseReceived(*this);
    }
  }
}

void Resource::AppendData(const std::string& bytes) {
  body_ += bytes;
  for (ResourceClient* client : clients_.ToVector()) {
    if (client) {
      client->DataReceived(*this);
    }
  }
}

void Resource::Finish(int net_error) {
  if (!IsLoading()) {
    return;
  }
  // ResourceLoader::DidReceiveResponse(): a status of 400 or more fails a
  // resource that does not ignore it.
  if (!net_error && response_.status >= 400 &&
      !IgnoresHttpStatusErrors(type_)) {
    net_error = kHttpResponseCodeFailure;
  }
  if (net_error) {
    status_ = ResourceStatus::kLoadError;
    net_error_ = net_error;
    // Nothing reads what came of a load that failed.
    body_.clear();
    body_.shrink_to_fit();
  } else if (type_ == ResourceType::kImage) {
    // An image fails as a decode error when no decoder reads its header.
    image_ = ReadImageHeader(body_, mime_type_);
    status_ = image_ ? ResourceStatus::kCached : ResourceStatus::kDecodeError;
    // The Page decodes no pixels, so nothing reads the body again.
    body_.clear();
    body_.shrink_to_fit();
  } else {
    status_ = ResourceStatus::kCached;
  }
  for (ResourceClient* client : clients_.ToVector()) {
    if (client) {
      client->NotifyFinished(*this);
    }
  }
}

}  // namespace mitto
