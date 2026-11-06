#include "mitto/body_stream.h"

#include <algorithm>
#include <utility>

#include "base/numerics/safe_conversions.h"
#include "net/base/net_errors.h"

namespace mitto {
namespace {

// How much of the body stays here, for //net to send again on a new connection
// when a reused one turns out to be gone. This is what Chrome keeps of a page's
// stream, ChunkedDataPipeUploadDataStream::kDefaultDestinationWindowSize.
constexpr size_t kKeptSize = 65535;

}  // namespace

BodyStream::BodyStream(scoped_refptr<RequestChannel> channel,
                       std::optional<uint64_t> size,
                       Delegate& delegate)
    // A body that can only be read once has a null source in Fetch's terms, which
    // //net takes to mean that a 421 cannot be sent again. There is no identifier
    // to cache a response by, as for any other body.
    : net::UploadDataStream(/*is_chunked=*/!size.has_value(),
                            /*has_null_source=*/true,
                            /*identifier=*/0),
      channel_(std::move(channel)),
      size_(size),
      delegate_(delegate) {}

BodyStream::~BodyStream() {
  channel_->CloseBody();
}

void BodyStream::OnCallerAnswered() {
  if (!waiting_buffer_) {
    return;
  }
  const scoped_refptr<net::IOBuffer> buffer = std::move(waiting_buffer_);
  delegate_->OnWaitForCallerEnded();
  const int result = ReadInternal(
      buffer.get(), base::checked_cast<int>(waiting_buffer_size_));
  if (result != net::ERR_IO_PENDING) {
    OnReadCompleted(result);
  }
}

bool BodyStream::AllowHTTP1() const {
  return !is_chunked();
}

int BodyStream::InitInternal(const net::NetLogWithSource& net_log) {
  if (!all_bytes_kept_) {
    return net::ERR_UPLOAD_STREAM_REWIND_NOT_SUPPORTED;
  }
  if (size_) {
    SetSize(*size_);
  }
  return net::OK;
}

int BodyStream::ReadInternal(net::IOBuffer* buf, int buf_len) {
  const base::span<uint8_t> output =
      buf->first(base::checked_cast<size_t>(buf_len));

  // On another attempt, what the caller gave before comes first.
  const size_t position = base::checked_cast<size_t>(this->position());
  if (position < kept_bytes_.size()) {
    const size_t size = std::min(output.size(), kept_bytes_.size() - position);
    output.first(size).copy_from(
        base::span(kept_bytes_).subspan(position, size));
    return base::checked_cast<int>(size);
  }

  bool event_pushed = false;
  const int result = channel_->TakeBody(output, event_pushed);
  if (event_pushed) {
    delegate_->OnBodyWanted();
  }
  if (result == net::ERR_IO_PENDING) {
    waiting_buffer_ = buf;
    waiting_buffer_size_ = output.size();
    delegate_->OnWaitForCallerStarted();
    return net::ERR_IO_PENDING;
  }
  if (result > 0) {
    KeepForAnotherAttempt(output.first(base::checked_cast<size_t>(result)));
    return result;
  }

  // The caller has ended the body. If it promised more, this is what //net says
  // of a file that turns out shorter than it was.
  if (size_) {
    return net::ERR_UPLOAD_FILE_CHANGED;
  }
  // The end never comes in the same read as the last bytes of the body, as the
  // Baselines of a page's stream show: over HTTP/2, a last DATA frame with nothing
  // in it.
  SetIsFinalChunk();
  return 0;
}

void BodyStream::ResetInternal() {
  waiting_buffer_ = nullptr;
  waiting_buffer_size_ = 0;
}

void BodyStream::KeepForAnotherAttempt(base::span<const uint8_t> bytes) {
  if (kept_bytes_.size() >= kKeptSize) {
    all_bytes_kept_ = false;
    return;
  }
  kept_bytes_.insert(kept_bytes_.end(), bytes.begin(), bytes.end());
}

}  // namespace mitto
