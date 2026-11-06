#ifndef MITTO_BODY_STREAM_H_
#define MITTO_BODY_STREAM_H_

#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <vector>

#include "base/containers/span.h"
#include "base/memory/raw_ref.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "mitto/request_channel.h"
#include "net/base/io_buffer.h"
#include "net/base/upload_data_stream.h"
#include "net/log/net_log_with_source.h"

namespace mitto {

// A request's body that comes from its caller as //net asks for it, so a body of
// any size takes no more memory than a few buffers. It lives on the network
// thread, and what the caller writes waits in the RequestChannel.
//
// This is to a request what network::ChunkedDataPipeUploadDataStream is to a
// page's fetch() with a ReadableStream. net::ChunkedUploadDataStream will not do:
// it keeps every byte until the request ends, and it lets a stream go over
// HTTP/1.1.
//
// With no size this is a chunked upload, which //net refuses over HTTP/1.1 with
// ERR_ALPN_NEGOTIATION_FAILED, as it does for Chrome. With a size //net sends a
// Content-Length and uses any protocol, as for a File in Chrome.
class BodyStream final : public net::UploadDataStream {
 public:
  // What the request that owns the stream needs to hear of. It outlives the
  // stream, since the stream belongs to its net::URLRequest.
  class Delegate {
   public:
    // The channel holds a new event for the caller.
    virtual void OnBodyWanted() = 0;

    // A read has begun or stopped waiting for the caller, so a timer that stands
    // for a wait for the server has to stop or start.
    virtual void OnWaitForCallerStarted() = 0;
    virtual void OnWaitForCallerEnded() = 0;

   protected:
    virtual ~Delegate() = default;
  };

  BodyStream(scoped_refptr<RequestChannel> channel,
             std::optional<uint64_t> size,
             Delegate& delegate);

  BodyStream(const BodyStream&) = delete;
  BodyStream& operator=(const BodyStream&) = delete;

  ~BodyStream() override;

  base::WeakPtr<BodyStream> GetWeakPtr() {
    return weak_ptr_factory_.GetWeakPtr();
  }

  // The caller has written or ended the body, so a read that waits can go on.
  // This may delete the stream, so nothing may touch it afterwards.
  void OnCallerAnswered();

  // net::UploadDataStream:
  bool AllowHTTP1() const override;

 private:
  // net::UploadDataStream:
  int InitInternal(const net::NetLogWithSource& net_log) override;
  int ReadInternal(net::IOBuffer* buf, int buf_len) override;
  void ResetInternal() override;

  void KeepForAnotherAttempt(base::span<const uint8_t> bytes);

  const scoped_refptr<RequestChannel> channel_;
  const std::optional<uint64_t> size_;
  const raw_ref<Delegate> delegate_;

  // The start of the body, for another attempt on a new connection.
  std::vector<uint8_t> kept_bytes_;
  bool all_bytes_kept_ = true;

  // The read that waits for the caller, if any.
  scoped_refptr<net::IOBuffer> waiting_buffer_;
  size_t waiting_buffer_size_ = 0;

  base::WeakPtrFactory<BodyStream> weak_ptr_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_BODY_STREAM_H_
