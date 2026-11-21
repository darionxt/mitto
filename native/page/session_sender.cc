#include "mitto/page/session_sender.h"

#include <utility>

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
  if (forgotten_.contains(context)) {
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
  if (forgotten_.contains(context)) {
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

void SessionSender::SendContextGone(uint64_t context) {
  const base::AutoLock lock(lock_);
  if (forgotten_.contains(context)) {
    return;
  }
  ForgetUnder(context);
  send_.Run(EncodeContextGone(context));
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

}  // namespace mitto
