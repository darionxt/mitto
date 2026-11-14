// What a document's Cache API asks of the Session that loaded it, as Blink's
// CacheStorage and Cache ask Chrome's browser over Mojo. The Page program
// gives a document that the Session loaded over HTTP or HTTPS one, and the DOM
// knows nothing of how it reaches the Session, as with the CookieJar.
//
// Upstream: third_party/blink/public/mojom/cache_storage/cache_storage.mojom

#ifndef MITTO_DOM_CACHE_STORAGE_HOST_H_
#define MITTO_DOM_CACHE_STORAGE_HOST_H_

#include <stdint.h>

#include <memory>
#include <string>

#include "base/functional/callback.h"
#include "mitto/cache_storage_messages.h"

namespace mitto {

// Hears a stored body come, a part at a time, as the reader gives credit.
class StoredBodyClient {
 public:
  virtual ~StoredBodyClient() = default;

  virtual void OnBodyData(std::string bytes) = 0;
  // 0 when all of the body has come, and otherwise a net error. The host
  // destroys the client after it returns.
  virtual void OnBodyEnd(int net_error) = 0;
};

class CacheStorageHost {
 public:
  using Answer = base::OnceCallback<void(PageCacheStorageAnswer answer)>;

  // Asks the Session, with a number of the host's for the question, and calls
  // `answer` from a task of its own once it has answered, as a Mojo reply
  // comes. A call too large for a frame gets an answer of
  // kErrorQueryTooLarge, as one that the Session refused would.
  virtual void Ask(PageCacheStorageCall call, Answer answer) = 0;

  // Holds a put's body, which the Session takes a part at a time while it
  // writes the entry, until DropPutBody(). Returns its number.
  virtual uint64_t AddPutBody(std::string bytes) = 0;
  virtual void DropPutBody(uint64_t body) = 0;

  // A stored body of a response that a match found, which the host holds
  // with its `client` until all of it has come or ReleaseStoredBody().
  // Nothing of it comes before ReadStoredBody(), and then only as much as
  // CreditStoredBody() lets come beyond the first window.
  virtual void AdoptStoredBody(uint64_t body,
                               std::unique_ptr<StoredBodyClient> client) = 0;
  virtual void ReadStoredBody(uint64_t body) = 0;
  virtual void CreditStoredBody(uint64_t body, uint32_t bytes) = 0;
  // Destroys the body's client.
  virtual void ReleaseStoredBody(uint64_t body) = 0;

  // The Page no longer uses a cache that the Session opened for it.
  virtual void ReleaseCache(uint64_t cache) = 0;

  // The Page has made document `document`, which takes the place of the one
  // before it: what that one asked, held or opened goes, as its Mojo pipes go
  // with its context, and nothing of it reaches the Session from here on.
  virtual void DocumentCommitted(uint64_t document) = 0;

 protected:
  virtual ~CacheStorageHost() = default;
};

}  // namespace mitto

#endif  // MITTO_DOM_CACHE_STORAGE_HOST_H_
