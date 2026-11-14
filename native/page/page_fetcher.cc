#include "mitto/page/page_fetcher.h"

#include <utility>

#include "base/memory/raw_ptr.h"
#include "mitto/page_messages.h"

namespace mitto {
namespace {

// net::ERR_FAILED, which the Page program, with no //net, cannot name.
constexpr int kFailed = -2;

// How many bytes of the bodies of a document's keepalive requests may be in
// flight at once. One more fails before it goes.
// Upstream: third_party/blink/renderer/platform/loader/fetch/resource_fetcher.h
constexpr size_t kKeepaliveInflightBytesQuota = 64 * 1024;

PageRequestMode ToPageRequestMode(FetchMode mode) {
  switch (mode) {
    case FetchMode::kCors:
      return PageRequestMode::kCors;
    case FetchMode::kSameOrigin:
      return PageRequestMode::kSameOrigin;
    case FetchMode::kNoCors:
      return PageRequestMode::kNoCors;
  }
}

PageCredentialsMode ToPageCredentialsMode(FetchCredentialsMode credentials) {
  switch (credentials) {
    case FetchCredentialsMode::kOmit:
      return PageCredentialsMode::kOmit;
    case FetchCredentialsMode::kSameOrigin:
      return PageCredentialsMode::kSameOrigin;
    case FetchCredentialsMode::kInclude:
      return PageCredentialsMode::kInclude;
  }
}

PageReferrerPolicy ToPageReferrerPolicy(FetchReferrerPolicy policy) {
  switch (policy) {
    case FetchReferrerPolicy::kDefault:
      return PageReferrerPolicy::kDocument;
    case FetchReferrerPolicy::kNoReferrer:
      return PageReferrerPolicy::kNoReferrer;
    case FetchReferrerPolicy::kNoReferrerWhenDowngrade:
      return PageReferrerPolicy::kNoReferrerWhenDowngrade;
    case FetchReferrerPolicy::kOrigin:
      return PageReferrerPolicy::kOrigin;
    case FetchReferrerPolicy::kOriginWhenCrossOrigin:
      return PageReferrerPolicy::kOriginWhenCrossOrigin;
    case FetchReferrerPolicy::kSameOrigin:
      return PageReferrerPolicy::kSameOrigin;
    case FetchReferrerPolicy::kStrictOrigin:
      return PageReferrerPolicy::kStrictOrigin;
    case FetchReferrerPolicy::kStrictOriginWhenCrossOrigin:
      return PageReferrerPolicy::kStrictOriginWhenCrossOrigin;
    case FetchReferrerPolicy::kUnsafeUrl:
      return PageReferrerPolicy::kUnsafeUrl;
  }
}

// A request's priority byte: one more than the net::RequestPriority that
// Blink's WebURLRequest::ConvertToNetPriority() gives, which the Page program,
// with no //net, names by number: IDLE 1, LOWEST 2, LOW 3, MEDIUM 4 and
// HIGHEST 5.
// Upstream: third_party/blink/renderer/platform/exported/web_url_request.cc
uint8_t ToPagePriority(FetchPriority priority) {
  switch (priority) {
    case FetchPriority::kVeryLow:
      return 1 + 1;
    case FetchPriority::kLow:
      return 1 + 2;
    case FetchPriority::kMedium:
      return 1 + 3;
    case FetchPriority::kHigh:
      return 1 + 4;
    case FetchPriority::kVeryHigh:
      return 1 + 5;
  }
}

}  // namespace

// One fetch, between the Page's requests and the DOM's client.
class PageFetcher::Request final : public PageRequests::Client,
                                   public FetchController {
 public:
  Request(PageFetcher& fetcher,
          std::unique_ptr<FetchClient> client,
          bool keepalive)
      : fetcher_(fetcher), client_(std::move(client)), keepalive_(keepalive) {}

  Request(const Request&) = delete;
  Request& operator=(const Request&) = delete;

  ~Request() override = default;

  FetchClient& client() { return *client_; }
  uint64_t number() const { return number_; }
  void set_number(uint64_t number) { number_ = number; }
  bool keepalive() const { return keepalive_; }
  // The bytes of its body that count against the keepalive quota.
  size_t keepalive_bytes() const { return keepalive_bytes_; }
  void set_keepalive_bytes(size_t bytes) { keepalive_bytes_ = bytes; }

  // PageRequests::Client:
  void OnResponseStarted(const PageResponseStarted& response) override {
    FetchResponseHead head;
    head.status = response.status;
    head.status_text = response.status_text;
    head.url = response.url;
    head.redirected = response.redirected;
    head.cors = response.cors;
    head.opaque = response.opaque;
    head.headers = response.headers;
    client_->OnResponseStarted(head);
  }
  void OnResponseBody(std::string bytes) override {
    client_->OnResponseBody(std::move(bytes));
  }
  void OnResponseEnded(int net_error) override {
    client_->OnResponseEnded(net_error);
    // Destroys this.
    fetcher_->Finished(number_);
  }

  // FetchController:
  void TookBody(size_t bytes) override {
    fetcher_->requests_->TookBody(number_, bytes);
  }
  void Cancel() override {
    fetcher_->requests_->Cancel(number_);
    // Destroys this.
    fetcher_->Finished(number_);
  }

 private:
  const raw_ref<PageFetcher> fetcher_;
  const std::unique_ptr<FetchClient> client_;
  const bool keepalive_;
  size_t keepalive_bytes_ = 0;
  uint64_t number_ = 0;
};

PageFetcher::PageFetcher(PageRequests& requests) : requests_(requests) {}

PageFetcher::~PageFetcher() {
  // A keepalive request goes on without its document, which lets it go
  // without a word: the Session lets it outlive the document once it hears
  // of the next one.
  for (const auto& [number, request] : open_) {
    if (request->keepalive()) {
      requests_->LetGo(number);
    } else {
      requests_->Cancel(number);
    }
  }
}

void PageFetcher::Fetch(FetchRequestData data,
                        std::unique_ptr<FetchClient> client) {
  auto request =
      std::make_unique<Request>(*this, std::move(client), data.keepalive);
  Request& started = *request;
  started.client().OnStarted(started);
  // A request has to fit in one frame, so a body that leaves it no room, of
  // nearly 16 MiB, fails as a request that cannot be sent does: a Known Gap.
  // Beside its URL, method, header lines, referrer and body, the frame holds
  // 48 bytes: the document's ID, the number, seven bytes of kind, flags,
  // modes, policy and priority, the target, four sizes and the byte that says
  // whether a body follows.
  const size_t body_size = data.body ? data.body->size() : 0;
  const bool has_referrer =
      data.referrer != kClientReferrer && data.referrer != kNoReferrer;
  const size_t size = 48 + data.url.size() + data.method.size() +
                      data.headers.size() +
                      (has_referrer ? data.referrer.size() : 0) + body_size;
  // ResourceFetcher::StartLoad() fails a keepalive request whose body would
  // take the document's keepalive bodies past their quota.
  const bool over_quota =
      data.keepalive && data.body &&
      kKeepaliveInflightBytesQuota - keepalive_bytes_ < body_size;
  if (size > kMaxPayloadSize || over_quota) {
    started.client().OnResponseEnded(kFailed);
    return;
  }
  if (data.keepalive && data.body) {
    started.set_keepalive_bytes(body_size);
    keepalive_bytes_ += body_size;
  }
  PageRequest page_request;
  page_request.kind = data.destination == FetchDestination::kScript
                          ? PageRequestKind::kScript
                          : PageRequestKind::kFetch;
  page_request.url = std::move(data.url);
  page_request.method = std::move(data.method);
  page_request.headers = std::move(data.headers);
  page_request.body = std::move(data.body);
  if (data.mode) {
    page_request.mode = ToPageRequestMode(*data.mode);
  }
  if (data.credentials) {
    page_request.credentials = ToPageCredentialsMode(*data.credentials);
  }
  if (data.referrer == kNoReferrer) {
    page_request.no_referrer = true;
  } else if (has_referrer) {
    page_request.referrer = std::move(data.referrer);
  }
  page_request.referrer_policy = ToPageReferrerPolicy(data.referrer_policy);
  if (data.priority) {
    page_request.priority = ToPagePriority(*data.priority);
  }
  page_request.keepalive = data.keepalive;
  started.set_number(
      requests_->Start(std::move(page_request), started, data.render_blocking));
  open_.emplace(started.number(), std::move(request));
}

void PageFetcher::Finished(uint64_t number) {
  const auto found = open_.find(number);
  if (found == open_.end()) {
    return;
  }
  keepalive_bytes_ -= found->second->keepalive_bytes();
  open_.erase(found);
}

}  // namespace mitto
