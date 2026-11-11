// A Page's requests to its Session, on the Page program's main thread. It
// numbers them, hands each response to whoever asked for it, and gives the
// Session credit for a body only as its reader takes it, so that a reader that
// waits, such as a parser that waits for a script, slows the server down as a
// slow reader of a request does.

#ifndef MITTO_PAGE_PAGE_REQUESTS_H_
#define MITTO_PAGE_PAGE_REQUESTS_H_

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "mitto/page/page_reporter.h"
#include "mitto/page_messages.h"

namespace mitto {

class PageRequests {
 public:
  // Hears how one request goes.
  class Client {
   public:
    // The response, after any redirects.
    virtual void OnResponseStarted(const PageResponseStarted& response) = 0;
    // The next bytes of the body. The client calls TookBody() once it has
    // taken them, now or later, which gives the Session room for as many
    // more.
    virtual void OnResponseBody(std::string bytes) = 0;
    // The end: net::OK when all of the body has come, and otherwise why the
    // request failed, with or without a response. Nothing more comes.
    virtual void OnResponseEnded(int net_error) = 0;

   protected:
    virtual ~Client() = default;
  };

  // `send` sends a frame to the Session, and `refuse` refuses what it sent,
  // saying why.
  PageRequests(SendFrame send,
               base::RepeatingCallback<void(std::string why)> refuse);

  PageRequests(const PageRequests&) = delete;
  PageRequests& operator=(const PageRequests&) = delete;

  ~PageRequests();

  // Has `client` hear of the navigation that the Session makes when the Page
  // opens, if it makes one: its response is the Page's document.
  void SetNavigationClient(Client& client);

  // Asks the Session for a request of `kind` for `url`, with the Page's
  // document as its Initiator. `client` hears how it goes, and has to live
  // until its end.
  uint64_t Start(PageRequestKind kind, std::string url, Client& client);

  // The client of request `number` has taken `bytes` more of its body.
  void TookBody(uint64_t number, size_t bytes);

  // What the Session sends.
  void OnResponseStarted(const PageResponseStarted& response);
  void OnResponseBody(PageResponseBody body);
  void OnResponseEnded(const PageResponseEnded& ended);

 private:
  struct Open {
    raw_ptr<Client> client;
    // Bytes the Session has sent that the client has not taken yet: at most
    // kBodyWindow.
    size_t untaken = 0;
    bool started = false;
  };

  // Returns the request that the Session names, or null after refusing what
  // it sent.
  Open* Find(uint64_t number, std::string_view message);

  const SendFrame send_;
  const base::RepeatingCallback<void(std::string)> refuse_;
  std::map<uint64_t, Open> open_;
  raw_ptr<Client> navigation_client_ = nullptr;
  bool navigation_seen_ = false;
  uint64_t next_number_ = kNavigationRequest + 1;
};

}  // namespace mitto

#endif  // MITTO_PAGE_PAGE_REQUESTS_H_
