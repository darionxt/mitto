#include "mitto/page/page_requests.h"

#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"

namespace mitto {

PageRequests::PageRequests(
    SendFrame send,
    base::RepeatingCallback<void(std::string why)> refuse)
    : send_(std::move(send)), refuse_(std::move(refuse)) {}

PageRequests::~PageRequests() = default;

void PageRequests::SetNavigationClient(Client& client) {
  navigation_client_ = &client;
}

uint64_t PageRequests::Start(PageRequestKind kind,
                             std::string url,
                             Client& client) {
  PageRequest request;
  request.number = next_number_++;
  request.kind = kind;
  request.url = std::move(url);
  open_[request.number].client = &client;
  send_.Run(EncodeRequest(request));
  return request.number;
}

void PageRequests::TookBody(uint64_t number, size_t bytes) {
  const auto found = open_.find(number);
  // Once a request has ended, the Session needs no more credit for it.
  if (found == open_.end() || bytes == 0) {
    return;
  }
  CHECK_LE(bytes, found->second.untaken);
  found->second.untaken -= bytes;
  PageCredit credit;
  credit.number = number;
  credit.bytes = base::checked_cast<uint32_t>(bytes);
  send_.Run(EncodeCredit(credit));
}

PageRequests::Open* PageRequests::Find(uint64_t number,
                                       std::string_view message) {
  const auto found = open_.find(number);
  if (found == open_.end()) {
    refuse_.Run(
        base::StrCat({message, " of request ", base::NumberToString(number),
                      ", which is not in progress"}));
    return nullptr;
  }
  return &found->second;
}

void PageRequests::OnResponseStarted(const PageResponseStarted& response) {
  if (response.number == kNavigationRequest && !navigation_seen_ &&
      navigation_client_) {
    // The Session makes the navigation itself, at most once.
    navigation_seen_ = true;
    open_[kNavigationRequest].client = navigation_client_;
  }
  Open* const open = Find(response.number, "a response");
  if (!open) {
    return;
  }
  if (open->started) {
    refuse_.Run(base::StrCat({"a second response of request ",
                              base::NumberToString(response.number)}));
    return;
  }
  open->started = true;
  open->client->OnResponseStarted(response);
}

void PageRequests::OnResponseBody(PageResponseBody body) {
  Open* const open = Find(body.number, "a body");
  if (!open) {
    return;
  }
  if (!open->started || body.bytes.size() > kBodyWindow - open->untaken) {
    refuse_.Run(base::StrCat({"more of the body of request ",
                              base::NumberToString(body.number),
                              " than the Page has given credit for"}));
    return;
  }
  open->untaken += body.bytes.size();
  // The client may take the bytes, and give credit, before this returns.
  open->client->OnResponseBody(std::move(body.bytes));
}

void PageRequests::OnResponseEnded(const PageResponseEnded& ended) {
  Open* const open = Find(ended.number, "the end");
  if (!open) {
    return;
  }
  Client* const client = open->client;
  open_.erase(ended.number);
  client->OnResponseEnded(ended.net_error);
}

}  // namespace mitto
