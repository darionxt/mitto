#include "mitto/page/session_cache_storage.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "mitto/dom/event_loop.h"
#include "mitto/page_messages.h"

namespace mitto {
namespace {

// The answer that a question gets when it cannot be asked, as a call that
// the Session refused would: kErrorQueryTooLarge, an empty list or false.
PageCacheStorageAnswer TooLargeAnswer(const PageCacheStorageCall& call) {
  PageCacheStorageAnswer answer;
  answer.question = call.question;
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
SessionCacheStorage::~SessionCacheStorage() = default;

void SessionCacheStorage::SetSend(
    SendFrame send,
    base::RepeatingCallback<void(std::string why)> refuse) {
  send_ = std::move(send);
  refuse_ = std::move(refuse);
}

void SessionCacheStorage::Send(const PageCacheStorageCall& call) {
  send_.Run(EncodeFrame(PageMessage::kCacheStorage,
                        EncodeCacheStorageCall(call)));
}

void SessionCacheStorage::Ask(PageCacheStorageCall call, Answer answer) {
  call.question = ++last_question_;
  if (!CacheStorageCallFits(call)) {
    EventLoop::Get()
        ->TaskRunner(TaskType::kMiscPlatformAPI)
        ->PostTask(FROM_HERE,
                   base::BindOnce(std::move(answer), TooLargeAnswer(call)));
    return;
  }
  questions_.emplace(call.question, std::move(answer));
  Send(call);
}

uint64_t SessionCacheStorage::AddPutBody(std::string bytes) {
  const uint64_t body = ++last_put_body_;
  put_bodies_.emplace(body, std::move(bytes));
  return body;
}

void SessionCacheStorage::DropPutBody(uint64_t body) {
  put_bodies_.erase(body);
}

void SessionCacheStorage::AdoptStoredBody(
    uint64_t body,
    std::unique_ptr<StoredBodyClient> client) {
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
  PageCacheStorageCall call;
  call.op = CacheStorageCallOp::kReleaseBody;
  call.body = body;
  Send(call);
}

void SessionCacheStorage::ReleaseCache(uint64_t cache) {
  PageCacheStorageCall call;
  call.op = CacheStorageCallOp::kReleaseCache;
  call.cache = cache;
  Send(call);
}

void SessionCacheStorage::OnAnswer(PageCacheStorageAnswer answer) {
  switch (answer.op) {
    case CacheStorageAnswerOp::kBodyWanted: {
      const auto found = put_bodies_.find(answer.body);
      if (found == put_bodies_.end()) {
        refuse_.Run(base::StrCat({"a request for Cache put body ",
                                  base::NumberToString(answer.body),
                                  ", which no put holds"}));
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
  Answer callback = std::move(found->second);
  questions_.erase(found);
  std::move(callback).Run(std::move(answer));
}

}  // namespace mitto
