#include "mitto/page/session_sender.h"

#include <utility>

#include "base/containers/map_util.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"

namespace mitto {

namespace {

// The number of the request whose response `frame` is part of.
uint64_t RequestOf(const FrameFromSession& frame) {
  switch (frame.type) {
    case PageMessage::kResponseStarted:
      return frame.response.number;
    case PageMessage::kResponseBody:
      return frame.body.number;
    case PageMessage::kResponseEnded:
      return frame.ended.number;
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

uint64_t SessionSender::SendRequest(
    base::FunctionRef<std::string(uint64_t number)> encode,
    ResponseRoute route) {
  const base::AutoLock lock(lock_);
  // The Session's own navigation of the Page is request 0.
  const uint64_t number = ++last_request_;
  if (route) {
    routes_.emplace(number, std::move(route));
  }
  send_.Run(encode(number));
  return number;
}

uint64_t SessionSender::SendContext(
    base::FunctionRef<std::string(uint64_t id)> encode) {
  const base::AutoLock lock(lock_);
  const uint64_t id = ++last_context_;
  send_.Run(encode(id));
  return id;
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

uint64_t SessionSender::AddCacheStorageHost(CacheStorageRoute route) {
  const base::AutoLock lock(lock_);
  const uint64_t host = ++last_cache_storage_host_;
  cache_storage_hosts_.emplace(host, std::move(route));
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
    const CacheStorageRoute* const found =
        base::FindOrNull(cache_storage_hosts_, host);
    if (!found) {
      return std::nullopt;
    }
    route = *found;
  }
  route.Run(host, std::move(answer));
  return std::nullopt;
}

}  // namespace mitto
