// What a window's fetch() asks of the Page program: requests through the
// Session that loaded the window's document, with the document as their
// Initiator. The Page program gives the window one, and the DOM knows nothing
// of how it reaches the Session, as with the CookieJar.

#ifndef MITTO_DOM_FETCHER_H_
#define MITTO_DOM_FETCHER_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <optional>
#include <string>

namespace mitto {

// A request of a page's, once fetch() has checked it.
struct FetchRequestData {
  FetchRequestData();
  FetchRequestData(FetchRequestData&&);
  FetchRequestData& operator=(FetchRequestData&&);
  ~FetchRequestData();

  std::string method;
  // In UTF-8.
  std::string url;
  // The lines that the script set, each "name: value" and a line feed.
  std::string headers;
  std::optional<std::string> body;
};

// A response, after any redirects.
struct FetchResponseHead {
  FetchResponseHead();
  FetchResponseHead(const FetchResponseHead&);
  FetchResponseHead& operator=(const FetchResponseHead&);
  ~FetchResponseHead();

  int32_t status = 0;
  std::string status_text;
  // In UTF-8.
  std::string url;
  bool redirected = false;
  // Whether its tainting is cors, as the Session worked it out: a hop went to
  // another origin than the document's.
  bool cors = false;
  // Each "name: value" and a line feed, without Set-Cookie.
  std::string headers;
};

// What a request can be told while it runs.
class FetchController {
 public:
  // The client has taken `bytes` more of the body, which lets the Session
  // send as many more.
  virtual void TookBody(size_t bytes) = 0;
  // Ends the request, and destroys its client before it returns, so the
  // client must touch nothing of itself after calling it.
  virtual void Cancel() = 0;

 protected:
  virtual ~FetchController() = default;
};

// Hears how one request goes. The Fetcher owns it, and destroys it after its
// end or on Cancel().
class FetchClient {
 public:
  virtual ~FetchClient() = default;

  // First, before anything else: what the client can tell the request.
  virtual void OnStarted(FetchController& controller) = 0;
  virtual void OnResponseStarted(const FetchResponseHead& head) = 0;
  // The next bytes of the body, which the client takes now or later through
  // TookBody().
  virtual void OnResponseBody(std::string bytes) = 0;
  // 0 when all of the body has come, and otherwise a net error, before or
  // after the response started.
  virtual void OnResponseEnded(int net_error) = 0;
};

class Fetcher {
 public:
  virtual void Fetch(FetchRequestData request,
                     std::unique_ptr<FetchClient> client) = 0;

 protected:
  virtual ~Fetcher() = default;
};

}  // namespace mitto

#endif  // MITTO_DOM_FETCHER_H_
