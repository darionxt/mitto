#include "mitto/page/session_indexed_db.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "mitto/dom/event_loop.h"
#include "mitto/page_messages.h"

namespace mitto {
namespace {

// Whether `callback` ends its call, so that nothing more answers it.
bool EndsCall(const PageIndexedDbCallback& callback) {
  switch (callback.op) {
    case IndexedDbCallbackOp::kBlocked:
    case IndexedDbCallbackOp::kUpgradeNeeded:
      return false;
    case IndexedDbCallbackOp::kGetAllResult:
      return callback.done;
    default:
      return true;
  }
}

// What a call that cannot be sent hears instead, as an error of its own.
PageIndexedDbCallback TooLargeAnswer(const PageIndexedDbCall& call) {
  PageIndexedDbCallback answer;
  answer.call = call.call;
  answer.error = IndexedDbException::kUnknownError;
  answer.message = u"The serialized value is too large.";
  switch (call.op) {
    case IndexedDbCallOp::kPut:
      answer.op = IndexedDbCallbackOp::kPutResult;
      break;
    case IndexedDbCallOp::kGet:
      answer.op = IndexedDbCallbackOp::kGetResult;
      break;
    case IndexedDbCallOp::kGetAll:
      answer.op = IndexedDbCallbackOp::kGetAllResult;
      answer.done = true;
      break;
    case IndexedDbCallOp::kOpenCursor:
      answer.op = IndexedDbCallbackOp::kOpenCursorResult;
      break;
    case IndexedDbCallOp::kCursorAdvance:
    case IndexedDbCallOp::kCursorContinue:
    case IndexedDbCallOp::kCursorPrefetch:
      answer.op = IndexedDbCallbackOp::kCursorResult;
      break;
    case IndexedDbCallOp::kCount:
      answer.op = IndexedDbCallbackOp::kCountResult;
      break;
    case IndexedDbCallOp::kDeleteRange:
    case IndexedDbCallOp::kClear:
      answer.op = IndexedDbCallbackOp::kSuccessResult;
      break;
    case IndexedDbCallOp::kGetKeyGeneratorCurrentNumber:
      answer.op = IndexedDbCallbackOp::kKeyGeneratorResult;
      answer.number = -1;
      break;
    default:
      answer.op = IndexedDbCallbackOp::kError;
      break;
  }
  return answer;
}

}  // namespace

SessionIndexedDb::SessionIndexedDb() = default;
SessionIndexedDb::~SessionIndexedDb() = default;

void SessionIndexedDb::SetSend(
    SendFrame send,
    base::RepeatingCallback<void(std::string why)> refuse) {
  send_ = std::move(send);
  refuse_ = std::move(refuse);
}

uint64_t SessionIndexedDb::NextNumber() {
  return ++last_number_;
}

void SessionIndexedDb::Send(const PageIndexedDbCall& call) {
  // What a collected object of a document before lets go has gone with it.
  for (const uint64_t number : {call.connection, call.transaction, call.cursor}) {
    if (number != 0 && number <= last_number_before_) {
      return;
    }
  }
  send_.Run(EncodeIndexedDb(document_, call));
}

void SessionIndexedDb::Ask(PageIndexedDbCall call, Answer answer) {
  call.call = NextNumber();
  if (!IndexedDbCallFits(call)) {
    EventLoop::Get()
        ->TaskRunner(TaskType::kDatabaseAccess)
        ->PostTask(FROM_HERE,
                   base::BindOnce(std::move(answer), TooLargeAnswer(call)));
    return;
  }
  calls_.emplace(call.call, std::move(answer));
  Send(call);
}

void SessionIndexedDb::Tell(PageIndexedDbCall call) {
  // Only a put carries a value, and only a call with a number does.
  CHECK(IndexedDbCallFits(call));
  Send(call);
}

void SessionIndexedDb::ListenToConnection(uint64_t connection,
                                          Answer listener) {
  listeners_.insert_or_assign(connection, std::move(listener));
}

void SessionIndexedDb::StopListening(uint64_t connection) {
  listeners_.erase(connection);
}

void SessionIndexedDb::DocumentCommitted(uint64_t document) {
  document_ = document;
  calls_.clear();
  listeners_.clear();
  last_number_before_ = last_number_;
}

void SessionIndexedDb::OnCallback(PageIndexedDbCallback callback) {
  switch (callback.op) {
    case IndexedDbCallbackOp::kForcedClose:
    case IndexedDbCallbackOp::kVersionChange:
    case IndexedDbCallbackOp::kAbort:
    case IndexedDbCallbackOp::kComplete: {
      const auto found = listeners_.find(callback.connection);
      // A connection that the document closed hears nothing more.
      if (found != listeners_.end()) {
        found->second.Run(std::move(callback));
      }
      return;
    }
    default:
      break;
  }
  const auto found = calls_.find(callback.call);
  if (found == calls_.end() && callback.call <= last_number_before_) {
    // A call of a document before, which has gone.
    return;
  }
  if (found == calls_.end()) {
    refuse_.Run(base::StrCat({"an answer to IndexedDB call ",
                              base::NumberToString(callback.call),
                              ", which no script waits for"}));
    return;
  }
  Answer answer = EndsCall(callback) ? std::move(found->second) : found->second;
  if (EndsCall(callback)) {
    calls_.erase(found);
  }
  answer.Run(std::move(callback));
}

}  // namespace mitto
