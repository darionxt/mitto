// What a request on the network thread and whoever asked for it say to each
// other. A caller of the C API asks through a RequestChannel, and a Page
// through its PageHost; the request itself, its rules and its hops, are the
// same for both.

#ifndef MITTO_REQUEST_CLIENT_H_
#define MITTO_REQUEST_CLIENT_H_

#include <stddef.h>
#include <stdint.h>

#include "base/time/time.h"
#include "mitto/request_channel.h"
#include "net/base/io_buffer.h"

namespace mitto {

// What a request took, once it has ended, as Chrome's network service tells
// Blink when it completes: the body's bytes as they came over the wire, before
// any content coding was undone, every byte received, headers included, and
// when it ended.
struct RequestTotals {
  int64_t raw_body_bytes = 0;
  int64_t received_bytes = 0;
  base::TimeTicks completion_time;
};

// Hears how a request goes, on the network thread.
class RequestClient {
 public:
  virtual ~RequestClient() = default;

  // The response so far, as a MITTO_EVENT_RESPONSE_STARTED or
  // MITTO_EVENT_REDIRECT_FOLLOWED event describes it.
  virtual void OnResponseEvent(RequestEvent event) = 0;

  // Whether the response that `event` describes may go on: a redirect be
  // followed, or a response reach the client. One that may not ends the
  // request with net::ERR_FAILED, as Chrome's CORS check fails it, before
  // OnResponseEvent() hears of it. Called with the event of each.
  virtual bool AllowsResponse(const RequestEvent& event) = 0;

  // What the next read of the body goes into, and how many bytes it may take
  // at most. Nothing else touches the buffer during a read.
  virtual net::IOBuffer* read_buffer() = 0;
  virtual int read_size() const = 0;

  // `size` bytes of the body have arrived at the start of read_buffer(). No
  // more is read until the client asks again.
  virtual void OnBodyRead(size_t size) = 0;

  // The stream body wants more of what the caller writes. Only a caller of the
  // C API sends one.
  virtual void OnBodyWanted() {}

  // What the request took, just before OnFinished().
  virtual void OnTotals(const RequestTotals& totals) {}

  // The request has ended: net::OK when all of the body has come, and
  // otherwise a net error. Nothing more comes, and the client may go.
  virtual void OnFinished(int net_error) = 0;
};

// What whoever asked for a request can tell it, on the network thread.
class RunningRequest {
 public:
  // Reads the next part of the body into the client's buffer, once a response
  // has started.
  virtual void ReadBody() = 0;

  // Ends the request, and tells its client nothing more, because the client
  // is going.
  virtual void Abandon() = 0;

  // Goes on to the request's end with no client, reading and dropping what
  // its response holds, because its client is going and the request outlives
  // it, as Chrome's KeepAliveURLLoader finishes a keepalive request after its
  // renderer has gone.
  // Upstream: content/browser/loader/keep_alive_url_loader.h
  virtual void Detach() = 0;

 protected:
  virtual ~RunningRequest() = default;
};

}  // namespace mitto

#endif  // MITTO_REQUEST_CLIENT_H_
