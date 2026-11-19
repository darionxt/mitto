#include "mitto/page/session_storage_buckets.h"

#include <utility>

#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "mitto/page_messages.h"

namespace mitto {

SessionStorageBuckets::SessionStorageBuckets() = default;
SessionStorageBuckets::~SessionStorageBuckets() = default;

void SessionStorageBuckets::SetSend(
    SendFrame send,
    base::RepeatingCallback<void(std::string why)> refuse) {
  send_ = std::move(send);
  refuse_ = std::move(refuse);
}

void SessionStorageBuckets::Ask(PageStorageBucketCall call, Answer answer) {
  call.question = ++last_question_;
  questions_.emplace(call.question, std::move(answer));
  send_.Run(EncodeStorageBuckets(document_, call));
}

void SessionStorageBuckets::Release(uint64_t bucket) {
  // A bucket of a document before has gone with it.
  if (buckets_.erase(bucket) == 0) {
    return;
  }
  PageStorageBucketCall call;
  call.op = StorageBucketCallOp::kRelease;
  call.bucket = bucket;
  send_.Run(EncodeStorageBuckets(document_, call));
}

void SessionStorageBuckets::DocumentCommitted(uint64_t document) {
  document_ = document;
  questions_.clear();
  buckets_.clear();
  last_question_before_ = last_question_;
}

void SessionStorageBuckets::OnAnswer(PageStorageBucketAnswer answer) {
  const auto found = questions_.find(answer.question);
  if (found == questions_.end() && answer.question <= last_question_before_) {
    // A question of a document before, which has gone.
    return;
  }
  if (found == questions_.end()) {
    refuse_.Run(base::StrCat({"an answer to Storage Buckets question ",
                              base::NumberToString(answer.question),
                              ", which no script waits for"}));
    return;
  }
  if (answer.op == StorageBucketAnswerOp::kOpened &&
      answer.error == StorageBucketError::kNone) {
    buckets_.insert(answer.bucket);
  }
  Answer callback = std::move(found->second);
  questions_.erase(found);
  std::move(callback).Run(std::move(answer));
}

}  // namespace mitto
