#include "mitto/page/page_fetcher.h"

#include <utility>

#include "base/memory/raw_ptr.h"

namespace mitto {

// One fetch, between the Page's requests and the DOM's client.
class PageFetcher::Request final : public PageRequests::Client,
                                   public FetchController {
 public:
  Request(PageFetcher& fetcher, std::unique_ptr<FetchClient> client)
      : fetcher_(fetcher), client_(std::move(client)) {}

  Request(const Request&) = delete;
  Request& operator=(const Request&) = delete;

  ~Request() override = default;

  FetchClient& client() { return *client_; }
  uint64_t number() const { return number_; }
  void set_number(uint64_t number) { number_ = number; }

  // PageRequests::Client:
  void OnResponseStarted(const PageResponseStarted& response) override {
    FetchResponseHead head;
    head.status = response.status;
    head.status_text = response.status_text;
    head.url = response.url;
    head.redirected = response.redirected;
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
  uint64_t number_ = 0;
};

PageFetcher::PageFetcher(PageRequests& requests) : requests_(requests) {}

PageFetcher::~PageFetcher() {
  for (const auto& [number, request] : open_) {
    requests_->Cancel(number);
  }
}

void PageFetcher::Fetch(FetchRequestData data,
                        std::unique_ptr<FetchClient> client) {
  auto request = std::make_unique<Request>(*this, std::move(client));
  Request& started = *request;
  started.client().OnStarted(started);
  PageRequest page_request;
  page_request.kind = PageRequestKind::kFetch;
  page_request.url = std::move(data.url);
  page_request.method = std::move(data.method);
  page_request.headers = std::move(data.headers);
  page_request.body = std::move(data.body);
  started.set_number(requests_->Start(std::move(page_request), started));
  open_.emplace(started.number(), std::move(request));
}

void PageFetcher::Finished(uint64_t number) {
  open_.erase(number);
}

}  // namespace mitto
