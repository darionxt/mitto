#include "mitto/page/session_storage_buckets.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "mitto/page_messages.h"

namespace mitto {

SessionStorageBuckets::SessionStorageBuckets() = default;

SessionStorageBuckets::~SessionStorageBuckets() {
  if (host_ != 0) {
    sender_->RemoveStorageBucketHost(host_);
  }
}

void SessionStorageBuckets::SetSender(
    scoped_refptr<SessionSender> sender,
    scoped_refptr<base::SingleThreadTaskRunner> runner,
    base::RepeatingCallback<void(std::string why)> refuse) {
  sender_ = std::move(sender);
  runner_ = std::move(runner);
  refuse_ = std::move(refuse);
}

void SessionStorageBuckets::Ask(PageStorageBucketCall call, Answer answer) {
  const uint64_t question =
      sender_->SendStorageBucketQuestion(host_, [this, &call](uint64_t number) {
        call.question = number;
        return EncodeStorageBuckets(document_, call);
      });
  questions_.emplace(question, std::move(answer));
}

void SessionStorageBuckets::Release(uint64_t bucket) {
  // A bucket of a document before has gone with it.
  if (buckets_.erase(bucket) == 0) {
    return;
  }
  PageStorageBucketCall call;
  call.op = StorageBucketCallOp::kRelease;
  call.bucket = bucket;
  sender_->SendFromContext(document_, EncodeStorageBuckets(document_, call));
}

void SessionStorageBuckets::DocumentCommitted(uint64_t document) {
  // What the document before asked goes unheard.
  if (host_ != 0) {
    sender_->RemoveStorageBucketHost(host_);
  }
  host_ = sender_->AddStorageBucketHost(
      document, base::BindRepeating(
                    [](scoped_refptr<base::SingleThreadTaskRunner> runner,
                       base::WeakPtr<SessionStorageBuckets> self, uint64_t host,
                       PageStorageBucketAnswer answer) {
                      runner->PostTask(
                          FROM_HERE,
                          base::BindOnce(&SessionStorageBuckets::OnAnswer, self,
                                         host, std::move(answer)));
                    },
                    runner_, weak_factory_.GetWeakPtr()));
  document_ = document;
  questions_.clear();
  buckets_.clear();
}

void SessionStorageBuckets::OnAnswer(uint64_t host,
                                     PageStorageBucketAnswer answer) {
  if (host != host_) {
    // An answer to a document before, which has gone.
    return;
  }
  const auto found = questions_.find(answer.question);
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
