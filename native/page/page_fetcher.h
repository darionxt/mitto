// What a window's fetch() asks its Session through: each request a fetch
// Request Kind's, on the Page's requests, with the document as its Initiator.
// One serves one window, and cancels what it has open when it stops or goes,
// as a document's loaders are cancelled when it goes, but for keepalive
// requests, which it lets go on.

#ifndef MITTO_PAGE_PAGE_FETCHER_H_
#define MITTO_PAGE_PAGE_FETCHER_H_

#include <stdint.h>

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/memory/raw_ref.h"
#include "base/time/time.h"
#include "mitto/dom/fetcher.h"
#include "mitto/page/page_requests.h"

namespace mitto {

// The byte of a request's referrer policy, where kDefault leaves the
// document's to the Session.
PageReferrerPolicy ToPageReferrerPolicy(FetchReferrerPolicy policy);

class PageFetcher final : public Fetcher {
 public:
  // The fetcher of a document, or of a worker.
  enum class Kind { kDocument, kWorker };

  PageFetcher(PageRequests& requests, Kind kind);

  PageFetcher(const PageFetcher&) = delete;
  PageFetcher& operator=(const PageFetcher&) = delete;

  // Stops the requests that are open, as Stop() does.
  ~PageFetcher() override;

  // The context has gone: each open request is cancelled, or, if it is a
  // keepalive request or a worker's, let go, and its client hears nothing more.
  // The Session ends a worker's once it hears that the worker has gone, as the
  // network service does once the worker's thread has ended, so what a worker
  // asks for just before it closes is still sent. The clients go with the
  // fetcher, since one of them may be what ended the context, and still run.
  void Stop();
  // As Stop(), for the old document of a window that a document of its
  // origin keeps, whose context goes on: each client then hears that its
  // request failed, a keepalive one's too, whose request goes on, as Blink's
  // ResourceFetcher::StopFetching() fails the old document's loaders.
  void Abort();

  // Fetcher:
  void Fetch(FetchRequestData request,
             std::unique_ptr<FetchClient> client) override;
  void FetchSynchronously(FetchRequestData request,
                          base::TimeDelta timeout,
                          FetchClient& client) override;
  bool HasFinishedResponse(std::string_view url) const override;
  void PrefetchDns(const GURL& url) override;
  void Preconnect(const GURL& url, bool allow_credentials) override;

 private:
  class Request;

  // The Session's request for `data`, or nothing for one that takes more than
  // a frame holds, a Known Gap.
  static std::optional<PageRequest> ToPageRequest(FetchRequestData& data);

  // Destroys request `number` and its client.
  void Finished(uint64_t number);

  const raw_ref<PageRequests> requests_;
  std::map<uint64_t, std::unique_ptr<Request>> open_;
  // The requests that Stop() stopped.
  std::vector<std::unique_ptr<Request>> stopped_;
  const Kind kind_;
  // The bytes of the bodies of the open keepalive requests.
  size_t keepalive_bytes_ = 0;
  // The final URLs of the responses that have come whole, as the Session
  // keeps them for the document.
  std::set<std::string, std::less<>> finished_responses_;
};

}  // namespace mitto

#endif  // MITTO_PAGE_PAGE_FETCHER_H_
