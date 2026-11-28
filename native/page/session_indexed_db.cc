#include "mitto/page/session_indexed_db.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "mitto/page_messages.h"

namespace mitto {
namespace {

// Whether `callback` ends its call, so that nothing more answers it.
bool EndsCall(const PageIndexedDbCallback& callback) {
  switch (callback.op) {
    case IndexedDbCallbackOp::kBlocked:
    case IndexedDbCallbackOp::kUpgradeNeeded:
    case IndexedDbCallbackOp::kBlob:
      return false;
    case IndexedDbCallbackOp::kGetAllResult:
      return callback.done;
    default:
      return true;
  }
}

// What a call that cannot be sent hears instead, as an error of its own: the
// error that Blink's IDBTransaction::Put() gives one too large for a Mojo
// message, with the size of a frame, or of the blobs that a put may hold. Like
// Blink's, the Session does not count it among the errors it sent.
PageIndexedDbCallback TooLargeAnswer(const PageIndexedDbCall& call) {
  uint64_t size = call.value.BlobsSize();
  size_t max = kMaxIndexedDbBlobsSize;
  if (size <= max) {
    size = EncodeIndexedDbCall(call).size();
    max = kMaxPayloadSize - sizeof(uint64_t);
  }
  PageIndexedDbCallback answer;
  answer.call = call.call;
  answer.error = IndexedDbException::kUnknownError;
  answer.message =
      base::StrCat({u"The serialized keys and/or value are too large (size=",
                    base::NumberToString16(size), u" bytes, max=",
                    base::NumberToString16(max), u" bytes)."});
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

SessionIndexedDb::~SessionIndexedDb() {
  if (host_ != 0) {
    sender_->RemoveIndexedDbHost(host_);
  }
}

void SessionIndexedDb::SetSender(
    scoped_refptr<SessionSender> sender,
    scoped_refptr<base::SingleThreadTaskRunner> runner,
    base::RepeatingCallback<void(std::string why)> refuse) {
  sender_ = std::move(sender);
  runner_ = std::move(runner);
  refuse_ = std::move(refuse);
}

uint64_t SessionIndexedDb::NextNumber() {
  return sender_->NextIndexedDbNumber();
}

bool SessionIndexedDb::NamesWhatWent(const PageIndexedDbCall& call) const {
  for (const uint64_t number :
       {call.connection, call.transaction, call.cursor}) {
    if (number != 0 && number < first_number_) {
      return true;
    }
  }
  return false;
}

void SessionIndexedDb::Send(const PageIndexedDbCall& call) {
  // What a collected object of a document before lets go has gone with it.
  if (NamesWhatWent(call)) {
    return;
  }
  used_ = true;
  sender_->SendFromContext(document_, EncodeIndexedDb(document_, call));
}

void SessionIndexedDb::Ask(PageIndexedDbCall call, Answer answer) {
  if (call.op == IndexedDbCallOp::kOpen) {
    // The upgrade's transaction, if the open needs one.
    transaction_connections_.insert_or_assign(call.transaction,
                                              call.connection);
  }
  // Each number takes as many bytes, so the call's own does not change
  // whether it fits.
  if (!IndexedDbCallFits(call)) {
    runner_->PostTask(FROM_HERE,
                      base::BindOnce(std::move(answer), TooLargeAnswer(call)));
    return;
  }
  if (NamesWhatWent(call)) {
    return;
  }
  used_ = true;
  // What the Session tells the connection that an open makes may come before
  // the context has run the open's success, such as the versionchange of an
  // upgrade that waits behind it, and goes to the context after that success,
  // so its route is there from when the open goes.
  if (call.op == IndexedDbCallOp::kOpen) {
    sender_->AddIndexedDbConnection(host_, call.connection);
  }
  SendIndexedDbBlobs(call.value, [this, &call](IndexedDbBlobPart part) {
    PageIndexedDbCall blob;
    blob.op = IndexedDbCallOp::kPutBlob;
    blob.transaction = call.transaction;
    blob.blob_part = std::move(part);
    Send(blob);
  });
  const uint64_t number =
      sender_->SendIndexedDbCall(host_, [this, &call](uint64_t number) {
        call.call = number;
        return EncodeIndexedDb(document_, call);
      });
  calls_[number].answer = std::move(answer);
  if (call.op == IndexedDbCallOp::kOpen) {
    opens_.emplace(number, call.connection);
  }
}

void SessionIndexedDb::Tell(PageIndexedDbCall call) {
  if (call.op == IndexedDbCallOp::kCreateTransaction) {
    transaction_connections_.insert_or_assign(call.transaction,
                                              call.connection);
  }
  if (IndexedDbCallFits(call)) {
    Send(call);
    return;
  }
  // A name, a key path or index keys too large for a frame, which Chrome's
  // larger Mojo messages hold: the call's transaction aborts instead, as if
  // the page had aborted it.
  const auto found = transaction_connections_.find(call.transaction);
  if (found == transaction_connections_.end()) {
    return;
  }
  PageIndexedDbCall abort;
  abort.op = IndexedDbCallOp::kAbort;
  abort.connection = found->second;
  abort.transaction = call.transaction;
  Send(abort);
}

void SessionIndexedDb::ListenToConnection(uint64_t connection,
                                          Answer listener) {
  sender_->AddIndexedDbConnection(host_, connection);
  listeners_.insert_or_assign(connection, std::move(listener));
}

void SessionIndexedDb::StopListening(uint64_t connection) {
  sender_->ForgetIndexedDbConnection(connection);
  listeners_.erase(connection);
}

bool SessionIndexedDb::UsedByDocument() const {
  return used_;
}

void SessionIndexedDb::DocumentCommitted(uint64_t document) {
  used_ = false;
  // What the document before asked and opened goes unheard.
  if (host_ != 0) {
    sender_->RemoveIndexedDbHost(host_);
  }
  host_ = sender_->AddIndexedDbHost(
      document, base::BindRepeating(
                    [](scoped_refptr<base::SingleThreadTaskRunner> runner,
                       base::WeakPtr<SessionIndexedDb> self, uint64_t host,
                       PageIndexedDbCallback callback) {
                      runner->PostTask(
                          FROM_HERE,
                          base::BindOnce(&SessionIndexedDb::OnCallback, self,
                                         host, std::move(callback)));
                    },
                    runner_, weak_factory_.GetWeakPtr()));
  first_number_ = sender_->NextIndexedDbNumber();
  document_ = document;
  calls_.clear();
  listeners_.clear();
  opens_.clear();
  transaction_connections_.clear();
}

void SessionIndexedDb::OnCallback(uint64_t host,
                                  PageIndexedDbCallback callback) {
  if (host != host_) {
    // What the Session sent a document before, which has gone.
    return;
  }
  switch (callback.op) {
    case IndexedDbCallbackOp::kForcedClose:
    case IndexedDbCallbackOp::kVersionChange:
    case IndexedDbCallbackOp::kAbort:
    case IndexedDbCallbackOp::kComplete: {
      if (callback.op == IndexedDbCallbackOp::kAbort ||
          callback.op == IndexedDbCallbackOp::kComplete) {
        transaction_connections_.erase(callback.transaction);
      }
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
  if (found == calls_.end()) {
    refuse_.Run(base::StrCat({"an answer to IndexedDB call ",
                              base::NumberToString(callback.call),
                              ", which no script waits for"}));
    return;
  }
  const uint64_t call = callback.call;
  IndexedDbBlobs& blobs = found->second.blobs;
  if (callback.op == IndexedDbCallbackOp::kBlob) {
    if (!blobs.Add(std::move(callback.blob_part))) {
      refuse_.Run("a part of an IndexedDB blob that continues none");
    }
    return;
  }
  if (!blobs.PutInto(callback.Values())) {
    refuse_.Run("an IndexedDB value whose blobs did not come");
    return;
  }
  const bool ends = EndsCall(callback);
  Answer answer = ends ? std::move(found->second.answer) : found->second.answer;
  if (ends) {
    calls_.erase(found);
    sender_->ForgetIndexedDbCall(call);
  }
  answer.Run(std::move(callback));
  if (!ends) {
    return;
  }
  // An open that ended with no database that listens, as one that failed,
  // lets its connection's route go.
  const auto open = opens_.find(call);
  if (open == opens_.end()) {
    return;
  }
  if (!listeners_.contains(open->second)) {
    sender_->ForgetIndexedDbConnection(open->second);
  }
  opens_.erase(open);
}

}  // namespace mitto
