#include "mitto/page/page_requests.h"

#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/dom/performance.h"
#include "mitto/dom/window.h"
#include "mitto/page/resource_timing.h"

namespace mitto {

PageRequests::Timed::Timed() = default;
PageRequests::Timed::Timed(Timed&&) = default;
PageRequests::Timed& PageRequests::Timed::operator=(Timed&&) = default;
PageRequests::Timed::~Timed() = default;

PageRequests::Open::Open() = default;
PageRequests::Open::Open(Open&&) = default;
PageRequests::Open& PageRequests::Open::operator=(Open&&) = default;
PageRequests::Open::~Open() = default;

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
                             Client& client,
                             bool render_blocking) {
  PageRequest request;
  request.kind = kind;
  request.url = std::move(url);
  return Start(std::move(request), client, render_blocking);
}

uint64_t PageRequests::Start(PageRequest request,
                             Client& client,
                             bool render_blocking) {
  request.number = next_number_++;
  Open& open = open_[request.number];
  open.client = &client;
  // ResourceFetcher::RecordResourceTimingInfo(), when the page asks.
  Timed& timed = open.timed.emplace();
  timed.start_time = base::TimeTicks::Now();
  timed.initiator_type =
      request.kind == PageRequestKind::kScript ? u"script" : u"fetch";
  timed.url = base::UTF8ToUTF16(request.url);
  timed.render_blocking = render_blocking;
  timed.window = &Window::Current();
  send_.Run(EncodeRequest(request));
  return request.number;
}

void PageRequests::Cancel(uint64_t number) {
  const auto found = open_.find(number);
  if (found == open_.end() || !found->second.client) {
    return;
  }
  // ResourceFetcher::HandleLoaderError() adds the entry of a request that the
  // page let go, as of now.
  if (found->second.timed) {
    AddResourceTiming(*found->second.timed, base::TimeTicks::Now(), nullptr);
    found->second.timed.reset();
  }
  // It stays open until the Session ends it, so that what the Session sent
  // meanwhile still counts against its credit.
  found->second.client = nullptr;
  send_.Run(EncodeCancel(number));
}

// static
void PageRequests::AddResourceTiming(Timed& timed,
                                     base::TimeTicks response_end,
                                     const PageResponseEnded* ended) {
  Window* const window = timed.window.Get();
  if (!window || !window->HasContext()) {
    return;
  }
  const PageResponseStarted* const response =
      timed.response ? &*timed.response : nullptr;
  ResourceTimingInfo info = CreateResourceTimingInfo(
      timed.start_time, std::move(timed.url), response, response_end);
  if (response) {
    if (info.allow_timing_details) {
      info.last_redirect_end_time = TicksOf(response->last_redirect_end);
    }
    if (ended) {
      SetResourceTimingSizes(info, *response, ended->encoded_body_size,
                             ended->received_bytes, timed.decoded_body_size);
    }
  }
  info.render_blocking_status = timed.render_blocking;
  window->Performance()->AddResourceTiming(std::move(info),
                                           timed.initiator_type);
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
  if (open->timed) {
    open->timed->response = response;
  }
  if (open->client) {
    open->client->OnResponseStarted(response);
  }
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
  if (!open->client) {
    // Cancelled: the Session needs no credit for what it sent meanwhile.
    return;
  }
  if (open->timed) {
    open->timed->decoded_body_size += body.bytes.size();
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
  std::optional<Timed> timed = std::move(open->timed);
  open_.erase(ended.number);
  // ResourceFetcher adds the entry before the resource's clients hear of the
  // end, so that a script sees its own. A request that the Session refused
  // before it started never loaded, as Blink fails such a fetch first.
  if (timed && ended.completion_time != 0) {
    AddResourceTiming(*timed, TicksOf(ended.completion_time), &ended);
  }
  if (client) {
    client->OnResponseTotals(ended);
    client->OnResponseEnded(ended.net_error);
  }
}

}  // namespace mitto
