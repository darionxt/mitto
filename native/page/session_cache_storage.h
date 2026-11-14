// The Cache API of a Page's documents through its Session, on the Page
// program's main thread: each question goes over the channel with a number,
// and its answer comes back as a task of its own, on the queue of
// miscellaneous platform APIs that Blink binds CacheStorage to. A put's body
// stays here until the Session asks for it, a part at a time, and a stored
// body comes as its reader gives credit for it.
//
// Upstream: third_party/blink/renderer/modules/cache_storage/cache_storage.cc

#ifndef MITTO_PAGE_SESSION_CACHE_STORAGE_H_
#define MITTO_PAGE_SESSION_CACHE_STORAGE_H_

#include <stdint.h>

#include <map>
#include <set>
#include <memory>
#include <string>

#include "base/functional/callback.h"
#include "mitto/dom/cache_storage_host.h"
#include "mitto/page/page_reporter.h"
#include "mitto/page_messages.h"

namespace mitto {

class SessionCacheStorage final : public CacheStorageHost {
 public:
  SessionCacheStorage();
  SessionCacheStorage(const SessionCacheStorage&) = delete;
  SessionCacheStorage& operator=(const SessionCacheStorage&) = delete;
  ~SessionCacheStorage() override;

  // On the main thread, before the first question: how frames go to the
  // Session, and how the Page refuses what it sent.
  void SetSend(SendFrame send,
               base::RepeatingCallback<void(std::string why)> refuse);

  // CacheStorageHost, on the main thread:
  void Ask(PageCacheStorageCall call, Answer answer) override;
  uint64_t AddPutBody(std::string bytes) override;
  void DropPutBody(uint64_t body) override;
  void AdoptStoredBody(uint64_t body,
                       std::unique_ptr<StoredBodyClient> client) override;
  void ReadStoredBody(uint64_t body) override;
  void CreditStoredBody(uint64_t body, uint32_t bytes) override;
  void ReleaseStoredBody(uint64_t body) override;
  void ReleaseCache(uint64_t cache) override;
  void DocumentCommitted(uint64_t document) override;

  // On the main thread, from a task of its own: what the Session sent.
  void OnAnswer(PageCacheStorageAnswer answer);

 private:
  struct StoredBody {
    StoredBody();
    StoredBody(StoredBody&&);
    StoredBody& operator=(StoredBody&&);
    ~StoredBody();

    std::unique_ptr<StoredBodyClient> client;
    bool reading = false;
    // Bytes the Session has sent that the reader has not taken.
    uint32_t untaken = 0;
  };

  void Send(const PageCacheStorageCall& call);

  SendFrame send_;
  base::RepeatingCallback<void(std::string)> refuse_;
  // The Page's document, which every call names.
  uint64_t document_ = kNoDocument;
  uint64_t last_question_ = 0;
  std::map<uint64_t, Answer> questions_;
  uint64_t last_put_body_ = 0;
  std::map<uint64_t, std::string> put_bodies_;
  std::map<uint64_t, StoredBody> stored_bodies_;
  // The caches that the Session opened for the document.
  std::set<uint64_t> caches_;
  // The last question and put body of the documents before, whose answers
  // and requests may still be on their way, and go unheard.
  uint64_t last_question_before_ = 0;
  uint64_t last_put_body_before_ = 0;
};

}  // namespace mitto

#endif  // MITTO_PAGE_SESSION_CACHE_STORAGE_H_
