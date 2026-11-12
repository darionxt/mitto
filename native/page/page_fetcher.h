// What a window's fetch() asks its Session through: each request a fetch
// Request Kind's, on the Page's requests, with the document as its Initiator.
// One serves one window, and cancels what it has open when it goes, as a
// document's loaders are cancelled when it goes.

#ifndef MITTO_PAGE_PAGE_FETCHER_H_
#define MITTO_PAGE_PAGE_FETCHER_H_

#include <stdint.h>

#include <map>
#include <memory>

#include "base/memory/raw_ref.h"
#include "mitto/dom/fetcher.h"
#include "mitto/page/page_requests.h"

namespace mitto {

class PageFetcher final : public Fetcher {
 public:
  explicit PageFetcher(PageRequests& requests);

  PageFetcher(const PageFetcher&) = delete;
  PageFetcher& operator=(const PageFetcher&) = delete;

  ~PageFetcher() override;

  // Fetcher:
  void Fetch(FetchRequestData request,
             std::unique_ptr<FetchClient> client) override;

 private:
  class Request;

  // Destroys request `number` and its client.
  void Finished(uint64_t number);

  const raw_ref<PageRequests> requests_;
  std::map<uint64_t, std::unique_ptr<Request>> open_;
};

}  // namespace mitto

#endif  // MITTO_PAGE_PAGE_FETCHER_H_
