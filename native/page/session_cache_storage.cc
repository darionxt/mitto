#include "mitto/page/session_cache_storage.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "mitto/page_messages.h"

namespace mitto {
namespace {

// The answer that a question gets when it cannot be asked, as a call that
// the Session refused would: kErrorQueryTooLarge, an empty list or false.
PageCacheStorageAnswer TooLargeAnswer(const PageCacheStorageCall& call) {
  PageCacheStorageAnswer answer;
  answer.error = 6;
  switch (call.op) {
    case CacheStorageCallOp::kOpen:
      answer.op = CacheStorageAnswerOp::kOpened;
      break;
    case CacheStorageCallOp::kHas:
    case CacheStorageCallOp::kDelete:
      answer.op = CacheStorageAnswerOp::kBoolean;
      break;
    case CacheStorageCallOp::kKeys:
      answer.op = CacheStorageAnswerOp::kNames;
      break;
    case CacheStorageCallOp::kCacheKeys:
      answer.op = CacheStorageAnswerOp::kRequests;
      break;
    case CacheStorageCallOp::kCacheBatch:
      answer.op = CacheStorageAnswerOp::kBatchDone;
      break;
    case CacheStorageCallOp::kEstimate:
      answer.op = CacheStorageAnswerOp::kEstimated;
      break;
    default:
      answer.op = CacheStorageAnswerOp::kResponses;
      break;
  }
  return answer;
}

}  // namespace

SessionCacheStorage::StoredBody::StoredBody() = default;
SessionCacheStorage::StoredBody::StoredBody(StoredBody&&) = default;
SessionCacheStorage::StoredBody& SessionCacheStorage::StoredBody::operator=(
    StoredBody&&) = default;
SessionCacheStorage::StoredBody::~StoredBody() = default;

SessionCacheStorage::SessionCacheStorage() = default;

SessionCacheStorage::~SessionCacheStorage() {
  if (host_ != 0) {
    sender_->RemoveCacheStorageHost(host_);
  }
}

void SessionCacheStorage::SetSender(
    scoped_refptr<SessionSender> sender,
    scoped_refptr<base::SingleThreadTaskRunner> runner,
    base::RepeatingCallback<void(std::string why)> refuse) {
  sender_ = std::move(sender);
  runner_ = std::move(runner);
  refuse_ = std::move(refuse);
}

void SessionCacheStorage::Send(const PageCacheStorageCall& call) {
  sender_->Send(EncodeCacheStorage(document_, call));
}

void SessionCacheStorage::Ask(PageCacheStorageCall call, Answer answer) {
  if (!CacheStorageCallFits(call)) {
    runner_->PostTask(FROM_HERE,
                      base::BindOnce(std::move(answer), TooLargeAnswer(call)));
    return;
  }
  const uint64_t question =
      sender_->SendCacheStorageQuestion(host_, [this, &call](uint64_t number) {
        call.question = number;
        return EncodeCacheStorage(document_, call);
      });
  questions_.emplace(question, std::move(answer));
}

uint64_t SessionCacheStorage::AddPutBody(std::string bytes) {
  const uint64_t body = sender_->AddCacheStoragePutBody(host_);
  put_bodies_.emplace(body, std::move(bytes));
  return body;
}

void SessionCacheStorage::DropPutBody(uint64_t body) {
  sender_->DropCacheStoragePutBody(body);
  put_bodies_.erase(body);
}

void SessionCacheStorage::AdoptStoredBody(
    uint64_t body,
    std::unique_ptr<StoredBodyClient> client) {
  sender_->AddCacheStorageBody(host_, body);
  StoredBody& stored = stored_bodies_[body];
  stored.client = std::move(client);
}

void SessionCacheStorage::ReadStoredBody(uint64_t body) {
  const auto found = stored_bodies_.find(body);
  if (found == stored_bodies_.end() || found->second.reading) {
    return;
  }
  found->second.reading = true;
  PageCacheStorageCall call;
  call.op = CacheStorageCallOp::kReadBody;
  call.body = body;
  Send(call);
}

void SessionCacheStorage::CreditStoredBody(uint64_t body, uint32_t bytes) {
  const auto found = stored_bodies_.find(body);
  if (found == stored_bodies_.end() || bytes == 0) {
    return;
  }
  found->second.untaken -= std::min(found->second.untaken, bytes);
  PageCacheStorageCall call;
  call.op = CacheStorageCallOp::kBodyCredit;
  call.body = body;
  call.credit = bytes;
  Send(call);
}

void SessionCacheStorage::ReleaseStoredBody(uint64_t body) {
  if (stored_bodies_.erase(body) == 0) {
    return;
  }
  sender_->DropCacheStorageBody(body);
  PageCacheStorageCall call;
  call.op = CacheStorageCallOp::kReleaseBody;
  call.body = body;
  Send(call);
}

void SessionCacheStorage::ReleaseCache(uint64_t cache) {
  // A cache of a document before has gone with it.
  if (caches_.erase(cache) == 0) {
    return;
  }
  PageCacheStorageCall call;
  call.op = CacheStorageCallOp::kReleaseCache;
  call.cache = cache;
  Send(call);
}

void SessionCacheStorage::DocumentCommitted(uint64_t document) {
  // What the document before asked, held or opened goes unheard.
  if (host_ != 0) {
    sender_->RemoveCacheStorageHost(host_);
  }
  host_ = sender_->AddCacheStorageHost(base::BindRepeating(
      [](scoped_refptr<base::SingleThreadTaskRunner> runner,
         base::WeakPtr<SessionCacheStorage> self, uint64_t host,
         PageCacheStorageAnswer answer) {
        runner->PostTask(FROM_HERE,
                         base::BindOnce(&SessionCacheStorage::OnAnswer, self,
                                        host, std::move(answer)));
      },
      runner_, weak_factory_.GetWeakPtr()));
  document_ = document;
  questions_.clear();
  put_bodies_.clear();
  stored_bodies_.clear();
  caches_.clear();
}

void SessionCacheStorage::OnAnswer(uint64_t host,
                                   PageCacheStorageAnswer answer) {
  if (host != host_) {
    // An answer to a document before, which has gone.
    return;
  }
  switch (answer.op) {
    case CacheStorageAnswerOp::kBodyWanted: {
      const auto found = put_bodies_.find(answer.body);
      if (found == put_bodies_.end()) {
        // A put that was dropped while this was on the way.
        return;
      }
      std::string& bytes = found->second;
      const size_t size = std::min<size_t>(bytes.size(), answer.most);
      if (size == 0) {
        refuse_.Run(base::StrCat({"a request for more of Cache put body ",
                                  base::NumberToString(answer.body),
                                  " than it holds"}));
        return;
      }
      PageCacheStorageCall call;
      call.op = CacheStorageCallOp::kBodyPart;
      call.body = answer.body;
      call.bytes = bytes.substr(0, size);
      bytes.erase(0, size);
      Send(call);
      return;
    }
    case CacheStorageAnswerOp::kBodyData: {
      const auto found = stored_bodies_.find(answer.body);
      if (found == stored_bodies_.end()) {
        // The body was released while this was on the way.
        return;
      }
      StoredBody& stored = found->second;
      if (!stored.reading ||
          stored.untaken + answer.bytes.size() > kBodyWindow) {
        refuse_.Run(base::StrCat({"more of stored body ",
                                  base::NumberToString(answer.body),
                                  " than the Page has given credit for"}));
        return;
      }
      stored.untaken += static_cast<uint32_t>(answer.bytes.size());
      stored.client->OnBodyData(std::move(answer.bytes));
      return;
    }
    case CacheStorageAnswerOp::kBodyEnd: {
      const auto found = stored_bodies_.find(answer.body);
      if (found == stored_bodies_.end()) {
        return;
      }
      std::unique_ptr<StoredBodyClient> client = std::move(found->second.client);
      stored_bodies_.erase(found);
      client->OnBodyEnd(answer.net_error);
      return;
    }
    default:
      break;
  }
  const auto found = questions_.find(answer.question);
  if (found == questions_.end()) {
    refuse_.Run(base::StrCat({"an answer to Cache API question ",
                              base::NumberToString(answer.question),
                              ", which no script waits for"}));
    return;
  }
  if (answer.op == CacheStorageAnswerOp::kOpened && answer.error == 0) {
    caches_.insert(answer.cache);
  }
  Answer callback = std::move(found->second);
  questions_.erase(found);
  std::move(callback).Run(std::move(answer));
}

}  // namespace mitto
