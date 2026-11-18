#include "mitto/page/session_sender.h"

#include <utility>

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

}  // namespace mitto
