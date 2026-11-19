// The Storage Buckets API of a Page's documents through its Session, on the
// Page program's main thread: each question goes over the channel with a
// number, and its answer comes back as a task of its own, on the queue of
// miscellaneous platform APIs that Blink binds StorageBucketManager to.
//
// Upstream: third_party/blink/renderer/modules/buckets/storage_bucket_manager.cc

#ifndef MITTO_PAGE_SESSION_STORAGE_BUCKETS_H_
#define MITTO_PAGE_SESSION_STORAGE_BUCKETS_H_

#include <stdint.h>

#include <map>
#include <set>
#include <string>

#include "base/functional/callback.h"
#include "mitto/dom/storage_bucket_host.h"
#include "mitto/page/page_reporter.h"
#include "mitto/page_messages.h"

namespace mitto {

class SessionStorageBuckets final : public StorageBucketHost {
 public:
  SessionStorageBuckets();
  SessionStorageBuckets(const SessionStorageBuckets&) = delete;
  SessionStorageBuckets& operator=(const SessionStorageBuckets&) = delete;
  ~SessionStorageBuckets() override;

  // On the main thread, before the first question: how frames go to the
  // Session, and how the Page refuses what it sent.
  void SetSend(SendFrame send,
               base::RepeatingCallback<void(std::string why)> refuse);

  // StorageBucketHost, on the main thread:
  void Ask(PageStorageBucketCall call, Answer answer) override;
  void Release(uint64_t bucket) override;
  void DocumentCommitted(uint64_t document) override;

  // On the main thread, from a task of its own: what the Session sent.
  void OnAnswer(PageStorageBucketAnswer answer);

 private:
  SendFrame send_;
  base::RepeatingCallback<void(std::string)> refuse_;
  // The Page's document, which every call names.
  uint64_t document_ = kNoDocument;
  uint64_t last_question_ = 0;
  std::map<uint64_t, Answer> questions_;
  // The buckets that the Session gave the document.
  std::set<uint64_t> buckets_;
  // The last question of the documents before, whose answers may still be on
  // their way, and go unheard.
  uint64_t last_question_before_ = 0;
};

}  // namespace mitto

#endif  // MITTO_PAGE_SESSION_STORAGE_BUCKETS_H_
