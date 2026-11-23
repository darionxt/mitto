// The Cache API of a context through the Page's Session: of the Page's
// documents on the main thread, one after another, or of a dedicated worker on
// its thread. Each question goes over the channel with a number that the
// SessionSender takes for the whole Page, and its answer comes back as a task
// of its own, on the context's queue of miscellaneous platform APIs, which
// Blink binds CacheStorage to. A put's body stays here until the Session asks
// for it, a part at a time, and a stored body comes as its reader gives credit
// for it.
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
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "mitto/dom/cache_storage_host.h"
#include "mitto/page/session_sender.h"
#include "mitto/page_messages.h"

namespace mitto {

class SessionCacheStorage final : public CacheStorageHost {
 public:
  SessionCacheStorage();
  SessionCacheStorage(const SessionCacheStorage&) = delete;
  SessionCacheStorage& operator=(const SessionCacheStorage&) = delete;
  ~SessionCacheStorage() override;

  // On the context's thread, before the first question: what sends its
  // frames and routes their answers, the queue the answers come on, and how
  // the Page refuses what the Session sent.
  void SetSender(scoped_refptr<SessionSender> sender,
                 scoped_refptr<base::SingleThreadTaskRunner> runner,
                 base::RepeatingCallback<void(std::string why)> refuse);

  // CacheStorageHost, on the context's thread. A worker's commits once, as
  // its ID:
  void Ask(PageCacheStorageCall call, Answer answer) override;
  uint64_t AddPutBody(std::string bytes) override;
  void DropPutBody(uint64_t body) override;
  void AdoptStoredBody(uint64_t body,
                       std::unique_ptr<StoredBodyClient> client) override;
  void ReadStoredBody(uint64_t body) override;
  void CreditStoredBody(uint64_t body, uint32_t bytes) override;
  void ReleaseStoredBody(uint64_t body) override;
  void ReleaseCache(uint64_t cache) override;
  bool HasCache(uint64_t cache) const override;
  void DocumentCommitted(uint64_t document) override;

  // From a task of its own: what the Session answered the document or worker
  // that `host` stands for, which goes unheard once another has its place.
  void OnAnswer(uint64_t host, PageCacheStorageAnswer answer);

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

  scoped_refptr<SessionSender> sender_;
  scoped_refptr<base::SingleThreadTaskRunner> runner_;
  base::RepeatingCallback<void(std::string)> refuse_;
  // The document or worker, which every call names, and the sender's number
  // of its host.
  uint64_t document_ = kNoDocument;
  uint64_t host_ = 0;
  std::map<uint64_t, Answer> questions_;
  std::map<uint64_t, std::string> put_bodies_;
  std::map<uint64_t, StoredBody> stored_bodies_;
  // The caches that the Session opened for the context.
  std::set<uint64_t> caches_;
  base::WeakPtrFactory<SessionCacheStorage> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_PAGE_SESSION_CACHE_STORAGE_H_
