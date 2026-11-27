#include "mitto/page/session_sender.h"

#include <utility>

#include "base/containers/map_util.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"

namespace mitto {

namespace {

// The number of the request whose response, or credit for whose body,
// `frame` is.
uint64_t RequestOf(const FrameFromSession& frame) {
  switch (frame.type) {
    case PageMessage::kResponseStarted:
      return frame.response.number;
    case PageMessage::kResponseBody:
      return frame.body.number;
    case PageMessage::kResponseEnded:
      return frame.ended.number;
    case PageMessage::kRequestBodyCredit:
      return frame.request_body_credit.number;
    case PageMessage::kUploadProgress:
      return frame.upload_progress.number;
    default:
      return kNavigationRequest;
  }
}

}  // namespace

SessionSender::SessionSender(base::RepeatingCallback<void(std::string)> send)
    : send_(std::move(send)) {}

SessionSender::~SessionSender() = default;

void SessionSender::Send(std::string frame) {
  send_.Run(std::move(frame));
}

void SessionSender::SendFromContext(uint64_t context, std::string frame) {
  const base::AutoLock lock(lock_);
  if (forgotten_.contains(context) || stored_.contains(context)) {
    return;
  }
  send_.Run(std::move(frame));
}

uint64_t SessionSender::SendRequest(
    uint64_t context,
    base::FunctionRef<std::string(uint64_t number)> encode,
    ResponseRoute route) {
  const base::AutoLock lock(lock_);
  // The Session's own navigation of the Page is request 0.
  const uint64_t number = ++last_request_;
  if (forgotten_.contains(context) || stored_.contains(context)) {
    unsent_requests_.insert(number);
    return number;
  }
  if (route) {
    routes_.emplace(number, std::move(route));
  }
  send_.Run(encode(number));
  return number;
}

void SessionSender::SendForRequest(uint64_t number, std::string frame) {
  const base::AutoLock lock(lock_);
  if (unsent_requests_.contains(number)) {
    return;
  }
  send_.Run(std::move(frame));
}

uint64_t SessionSender::SendTopLevelDocument(
    base::FunctionRef<std::string(uint64_t id)> encode) {
  const base::AutoLock lock(lock_);
  const uint64_t id = ++last_context_;
  // A top-level document takes the place of the one before it and of
  // everything under it, as the Session has it.
  for (const auto& [context, parent] : parents_) {
    forgotten_.insert(context);
  }
  parents_.clear();
  parents_.emplace(id, kNoDocument);
  send_.Run(encode(id));
  return id;
}

uint64_t SessionSender::SendContext(
    uint64_t parent,
    base::FunctionRef<std::string(uint64_t id)> encode) {
  const base::AutoLock lock(lock_);
  const uint64_t id = ++last_context_;
  if (parent == kNoDocument || forgotten_.contains(parent)) {
    forgotten_.insert(id);
    return id;
  }
  parents_.emplace(id, parent);
  send_.Run(encode(id));
  return id;
}

uint64_t SessionSender::SendFrameDocument(
    uint64_t parent,
    uint64_t replaced,
    base::FunctionRef<std::string(uint64_t id)> encode) {
  const base::AutoLock lock(lock_);
  const uint64_t id = ++last_context_;
  if (parent == kNoDocument || forgotten_.contains(parent)) {
    forgotten_.insert(id);
    return id;
  }
  ForgetUnder(replaced);
  parents_.emplace(id, parent);
  send_.Run(encode(id));
  return id;
}

void SessionSender::SendContextGone(uint64_t context) {
  const base::AutoLock lock(lock_);
  if (forgotten_.contains(context)) {
    return;
  }
  ForgetUnder(context);
  send_.Run(EncodeContextGone(context));
}

void SessionSender::SendDocumentStored(uint64_t document) {
  const base::AutoLock lock(lock_);
  parents_.erase(document);
  stored_.insert(document);
  send_.Run(EncodeDocumentStored(document));
}

void SessionSender::SendDocumentRestored(uint64_t document) {
  const base::AutoLock lock(lock_);
  for (const auto& [context, parent] : parents_) {
    forgotten_.insert(context);
  }
  parents_.clear();
  stored_.erase(document);
  parents_.emplace(document, kNoDocument);
  send_.Run(EncodeDocumentRestored(document));
}

void SessionSender::SendDocumentEvicted(uint64_t document) {
  const base::AutoLock lock(lock_);
  stored_.erase(document);
  forgotten_.insert(document);
  send_.Run(EncodeDocumentEvicted(document));
}

void SessionSender::ForgetUnder(uint64_t context) {
  std::set<uint64_t> under = {context};
  // Each context comes after its parent, as the IDs do.
  for (const auto& [id, parent] : parents_) {
    if (under.contains(parent)) {
      under.insert(id);
    }
  }
  for (const uint64_t id : under) {
    parents_.erase(id);
    forgotten_.insert(id);
  }
}

bool SessionSender::Route(FrameFromSession& frame) {
  const uint64_t number = RequestOf(frame);
  ResponseRoute route;
  {
    const base::AutoLock lock(lock_);
    const auto found = routes_.find(number);
    if (found == routes_.end()) {
      return false;
    }
    route = frame.type == PageMessage::kResponseEnded ? std::move(found->second)
                                                      : found->second;
    if (frame.type == PageMessage::kResponseEnded) {
      routes_.erase(found);
    }
  }
  route.Run(std::move(frame));
  return true;
}

uint64_t SessionSender::AddCacheStorageHost(uint64_t context,
                                            CacheStorageRoute route) {
  const base::AutoLock lock(lock_);
  const uint64_t host = ++last_cache_storage_host_;
  cache_storage_hosts_.emplace(
      host, Host<CacheStorageRoute>{context, std::move(route)});
  return host;
}

void SessionSender::RemoveCacheStorageHost(uint64_t host) {
  const base::AutoLock lock(lock_);
  cache_storage_hosts_.erase(host);
  const auto of_host = [host](const auto& entry) {
    return entry.second == host;
  };
  std::erase_if(cache_storage_questions_, of_host);
  std::erase_if(cache_storage_put_bodies_, of_host);
  std::erase_if(cache_storage_bodies_, of_host);
}

uint64_t SessionSender::SendCacheStorageQuestion(
    uint64_t host,
    base::FunctionRef<std::string(uint64_t question)> encode) {
  const base::AutoLock lock(lock_);
  const uint64_t question = ++last_cache_storage_question_;
  if (OfForgotten(base::FindOrNull(cache_storage_hosts_, host))) {
    return question;
  }
  cache_storage_questions_.emplace(question, host);
  send_.Run(encode(question));
  return question;
}

uint64_t SessionSender::AddCacheStoragePutBody(uint64_t host) {
  const base::AutoLock lock(lock_);
  const uint64_t body = ++last_cache_storage_put_body_;
  cache_storage_put_bodies_.emplace(body, host);
  return body;
}

void SessionSender::DropCacheStoragePutBody(uint64_t body) {
  const base::AutoLock lock(lock_);
  cache_storage_put_bodies_.erase(body);
}

void SessionSender::AddCacheStorageBody(uint64_t host, uint64_t body) {
  const base::AutoLock lock(lock_);
  cache_storage_bodies_.insert_or_assign(body, host);
}

void SessionSender::DropCacheStorageBody(uint64_t body) {
  const base::AutoLock lock(lock_);
  cache_storage_bodies_.erase(body);
}

std::optional<std::string> SessionSender::RouteCacheStorage(
    PageCacheStorageAnswer answer) {
  CacheStorageRoute route;
  uint64_t host = 0;
  {
    const base::AutoLock lock(lock_);
    switch (answer.op) {
      case CacheStorageAnswerOp::kBodyWanted: {
        const auto found = cache_storage_put_bodies_.find(answer.body);
        if (found == cache_storage_put_bodies_.end()) {
          if (answer.body > last_cache_storage_put_body_) {
            return base::StrCat({"a request for Cache put body ",
                                 base::NumberToString(answer.body),
                                 ", which no put holds"});
          }
          // A put whose context has gone, or that was dropped.
          return std::nullopt;
        }
        host = found->second;
        break;
      }
      case CacheStorageAnswerOp::kBodyData:
      case CacheStorageAnswerOp::kBodyEnd: {
        const auto found = cache_storage_bodies_.find(answer.body);
        if (found == cache_storage_bodies_.end()) {
          // Released while this was on the way.
          return std::nullopt;
        }
        host = found->second;
        if (answer.op == CacheStorageAnswerOp::kBodyEnd) {
          cache_storage_bodies_.erase(found);
        }
        break;
      }
      default: {
        const auto found = cache_storage_questions_.find(answer.question);
        if (found == cache_storage_questions_.end()) {
          if (answer.question > last_cache_storage_question_) {
            return base::StrCat({"an answer to Cache API question ",
                                 base::NumberToString(answer.question),
                                 ", which no script waits for"});
          }
          // A question of a context that has gone.
          return std::nullopt;
        }
        host = found->second;
        cache_storage_questions_.erase(found);
        break;
      }
    }
    const Host<CacheStorageRoute>* const found =
        base::FindOrNull(cache_storage_hosts_, host);
    if (!found) {
      return std::nullopt;
    }
    route = found->route;
  }
  route.Run(host, std::move(answer));
  return std::nullopt;
}

uint64_t SessionSender::AddIndexedDbHost(uint64_t context,
                                         IndexedDbRoute route) {
  const base::AutoLock lock(lock_);
  const uint64_t host = ++last_indexed_db_host_;
  indexed_db_hosts_.emplace(host,
                            Host<IndexedDbRoute>{context, std::move(route)});
  return host;
}

void SessionSender::RemoveIndexedDbHost(uint64_t host) {
  const base::AutoLock lock(lock_);
  indexed_db_hosts_.erase(host);
  const auto of_host = [host](const auto& entry) {
    return entry.second == host;
  };
  std::erase_if(indexed_db_calls_, of_host);
  std::erase_if(indexed_db_connections_, of_host);
}

uint64_t SessionSender::NextIndexedDbNumber() {
  const base::AutoLock lock(lock_);
  return ++last_indexed_db_number_;
}

uint64_t SessionSender::SendIndexedDbCall(
    uint64_t host,
    base::FunctionRef<std::string(uint64_t call)> encode) {
  const base::AutoLock lock(lock_);
  const uint64_t call = ++last_indexed_db_number_;
  if (OfForgotten(base::FindOrNull(indexed_db_hosts_, host))) {
    return call;
  }
  indexed_db_calls_.emplace(call, host);
  send_.Run(encode(call));
  return call;
}

void SessionSender::ForgetIndexedDbCall(uint64_t call) {
  const base::AutoLock lock(lock_);
  indexed_db_calls_.erase(call);
}

void SessionSender::AddIndexedDbConnection(uint64_t host, uint64_t connection) {
  const base::AutoLock lock(lock_);
  indexed_db_connections_.insert_or_assign(connection, host);
}

void SessionSender::ForgetIndexedDbConnection(uint64_t connection) {
  const base::AutoLock lock(lock_);
  indexed_db_connections_.erase(connection);
}

std::optional<std::string> SessionSender::RouteIndexedDb(
    PageIndexedDbCallback callback) {
  IndexedDbRoute route;
  uint64_t host = 0;
  {
    const base::AutoLock lock(lock_);
    switch (callback.op) {
      case IndexedDbCallbackOp::kForcedClose:
      case IndexedDbCallbackOp::kVersionChange:
      case IndexedDbCallbackOp::kAbort:
      case IndexedDbCallbackOp::kComplete: {
        const uint64_t* const found =
            base::FindOrNull(indexed_db_connections_, callback.connection);
        if (!found) {
          // A connection that its context closed hears nothing more.
          return std::nullopt;
        }
        host = *found;
        break;
      }
      default: {
        const uint64_t* const found =
            base::FindOrNull(indexed_db_calls_, callback.call);
        if (!found) {
          if (callback.call > last_indexed_db_number_) {
            return base::StrCat({"an answer to IndexedDB call ",
                                 base::NumberToString(callback.call),
                                 ", which no script waits for"});
          }
          // A call of a context that has gone.
          return std::nullopt;
        }
        host = *found;
        break;
      }
    }
    const Host<IndexedDbRoute>* const found =
        base::FindOrNull(indexed_db_hosts_, host);
    if (!found) {
      return std::nullopt;
    }
    route = found->route;
  }
  route.Run(host, std::move(callback));
  return std::nullopt;
}

uint64_t SessionSender::AddStorageBucketHost(uint64_t context,
                                             StorageBucketRoute route) {
  const base::AutoLock lock(lock_);
  const uint64_t host = ++last_storage_bucket_host_;
  storage_bucket_hosts_.emplace(
      host, Host<StorageBucketRoute>{context, std::move(route)});
  return host;
}

void SessionSender::RemoveStorageBucketHost(uint64_t host) {
  const base::AutoLock lock(lock_);
  storage_bucket_hosts_.erase(host);
  std::erase_if(storage_bucket_questions_,
                [host](const auto& entry) { return entry.second == host; });
}

uint64_t SessionSender::SendStorageBucketQuestion(
    uint64_t host,
    base::FunctionRef<std::string(uint64_t question)> encode) {
  const base::AutoLock lock(lock_);
  const uint64_t question = ++last_storage_bucket_question_;
  if (OfForgotten(base::FindOrNull(storage_bucket_hosts_, host))) {
    return question;
  }
  storage_bucket_questions_.emplace(question, host);
  send_.Run(encode(question));
  return question;
}

std::optional<std::string> SessionSender::RouteStorageBucket(
    PageStorageBucketAnswer answer) {
  StorageBucketRoute route;
  uint64_t host = 0;
  {
    const base::AutoLock lock(lock_);
    const auto found = storage_bucket_questions_.find(answer.question);
    if (found == storage_bucket_questions_.end()) {
      if (answer.question > last_storage_bucket_question_) {
        return base::StrCat({"an answer to Storage Buckets question ",
                             base::NumberToString(answer.question),
                             ", which no script waits for"});
      }
      // A question of a context that has gone.
      return std::nullopt;
    }
    host = found->second;
    storage_bucket_questions_.erase(found);
    const Host<StorageBucketRoute>* const found_route =
        base::FindOrNull(storage_bucket_hosts_, host);
    if (!found_route) {
      return std::nullopt;
    }
    route = found_route->route;
  }
  route.Run(host, std::move(answer));
  return std::nullopt;
}

}  // namespace mitto
