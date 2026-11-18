#include "mitto/dom/resource.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "base/check.h"
#include "mitto/dom/resource_fetcher.h"

namespace mitto {

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

void Resource::ResponseReceived(int status, std::string mime_type) {
  http_status_ = status;
  mime_type_ = std::move(mime_type);
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
  if (net_error) {
    status_ = ResourceStatus::kLoadError;
    // Nothing reads what came of a load that failed.
    body_.clear();
    body_.shrink_to_fit();
  } else {
    switch (type_) {
      case ResourceType::kImage:
        // An image ignores its response's status, as ImageResource does,
        // and fails as a decode error when no decoder reads its header.
        image_ = ReadImageHeader(body_, mime_type_);
        status_ =
            image_ ? ResourceStatus::kCached : ResourceStatus::kDecodeError;
        // The Page decodes no pixels, so nothing reads the body again.
        body_.clear();
        body_.shrink_to_fit();
        break;
    }
  }
  for (ResourceClient* client : clients_.ToVector()) {
    if (client) {
      client->NotifyFinished(*this);
    }
  }
}

}  // namespace mitto
