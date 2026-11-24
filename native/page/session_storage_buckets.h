// The Storage Buckets API of a context through the Page's Session: of the
// Page's documents on the main thread, one after another, or of a dedicated
// worker on its thread. Each question goes over the channel with a number that
// the SessionSender takes for the whole Page, and its answer comes back as a
// task of its own, on the context's queue of miscellaneous platform APIs,
// which Blink binds StorageBucketManager to.
//
// Upstream: third_party/blink/renderer/modules/buckets/storage_bucket_manager.cc

#ifndef MITTO_PAGE_SESSION_STORAGE_BUCKETS_H_
#define MITTO_PAGE_SESSION_STORAGE_BUCKETS_H_

#include <stdint.h>

#include <map>
#include <set>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "mitto/dom/storage_bucket_host.h"
#include "mitto/page/session_sender.h"
#include "mitto/page_messages.h"

namespace mitto {

class SessionStorageBuckets final : public StorageBucketHost {
 public:
  SessionStorageBuckets();
  SessionStorageBuckets(const SessionStorageBuckets&) = delete;
  SessionStorageBuckets& operator=(const SessionStorageBuckets&) = delete;
  ~SessionStorageBuckets() override;

  // On the context's thread, before the first question: what sends its
  // frames and routes their answers, the queue the answers come on, and how
  // the Page refuses what the Session sent.
  void SetSender(scoped_refptr<SessionSender> sender,
                 scoped_refptr<base::SingleThreadTaskRunner> runner,
                 base::RepeatingCallback<void(std::string why)> refuse);

  // StorageBucketHost, on the context's thread. A worker's commits once, as
  // its ID:
  void Ask(PageStorageBucketCall call, Answer answer) override;
  void Release(uint64_t bucket) override;
  void DocumentCommitted(uint64_t document) override;
  bool UsedByDocument() const override;

  // From a task of its own: what the Session answered the document or worker
  // that `host` stands for, which goes unheard once another has its place.
  void OnAnswer(uint64_t host, PageStorageBucketAnswer answer);

 private:
  scoped_refptr<SessionSender> sender_;
  scoped_refptr<base::SingleThreadTaskRunner> runner_;
  base::RepeatingCallback<void(std::string)> refuse_;
  // The document or worker, which every call names, and the sender's number
  // of its host.
  uint64_t document_ = kNoDocument;
  // Whether the document has asked anything here.
  bool used_ = false;
  uint64_t host_ = 0;
  std::map<uint64_t, Answer> questions_;
  // The buckets that the Session gave the context.
  std::set<uint64_t> buckets_;
  base::WeakPtrFactory<SessionStorageBuckets> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_PAGE_SESSION_STORAGE_BUCKETS_H_
