#include "mitto/dom/inflate_transformer.h"

#include <stdint.h>

#include <limits>
#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "base/containers/span.h"
#include "base/strings/strcat.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_promise.h"
#include "mitto/dom/compression_format.h"
#include "mitto/dom/stream_chunks.h"
#include "mitto/dom/transform_stream_default_controller.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-typed-array.h"

namespace mitto {

namespace {

// Enqueues each buffer in order, which may run a page's script.
void EnqueueBuffers(const std::vector<v8::Local<v8::Uint8Array>>& buffers,
                    TransformStreamDefaultController* controller,
                    bindings::ExceptionState& exception_state) {
  for (const v8::Local<v8::Uint8Array> buffer : buffers) {
    controller->Enqueue(buffer, exception_state);
    if (exception_state.HadException()) {
      return;
    }
  }
}

}  // namespace

InflateTransformer::InflateTransformer(CompressionFormat format)
    : out_buffer_(kBufferSize), stream_{} {
  constexpr int kWindowBits = 15;
  constexpr int kUseGzip = 16;
  int err = Z_OK;
  switch (format) {
    case CompressionFormat::kDeflate:
      err = inflateInit2(&stream_, kWindowBits);
      break;
    case CompressionFormat::kGzip:
      err = inflateInit2(&stream_, kWindowBits + kUseGzip);
      break;
    case CompressionFormat::kDeflateRaw:
      err = inflateInit2(&stream_, -kWindowBits);
      break;
  }
  CHECK_EQ(Z_OK, err);
}

InflateTransformer::~InflateTransformer() {
  if (!was_flush_called_) {
    inflateEnd(&stream_);
  }
}

v8::Local<v8::Promise> InflateTransformer::Transform(
    v8::Local<v8::Value> chunk,
    TransformStreamDefaultController* controller,
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  base::span<const uint8_t> bytes;
  if (!BytesOfBufferSource(isolate, chunk, exception_state, &bytes)) {
    return {};
  }
  if (bytes.size() > std::numeric_limits<uint32_t>::max()) {
    exception_state.ThrowRangeError(
        "Buffer size exceeds maximum heap object size.");
    return {};
  }
  Inflate(bytes, /*finished=*/false, controller, exception_state);
  return bindings::ResolvedUndefinedPromise(isolate);
}

v8::Local<v8::Promise> InflateTransformer::Flush(
    TransformStreamDefaultController* controller,
    bindings::ExceptionState& exception_state) {
  CHECK(!was_flush_called_);
  was_flush_called_ = true;
  Inflate({}, /*finished=*/true, controller, exception_state);
  inflateEnd(&stream_);
  out_buffer_.clear();
  if (exception_state.HadException()) {
    return {};
  }
  if (!reached_end_) {
    exception_state.ThrowTypeError("Compressed input was truncated.");
  }
  return bindings::ResolvedUndefinedPromise(exception_state.GetIsolate());
}

void InflateTransformer::Inflate(base::span<const uint8_t> input,
                                 bool finished,
                                 TransformStreamDefaultController* controller,
                                 bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  if (reached_end_ && !input.empty()) {
    // zlib ignores data after the end of the stream, so it throws here.
    exception_state.ThrowTypeError("Junk found after end of compressed data.");
    return;
  }
  stream_.avail_in = static_cast<uInt>(input.size());
  // zlib treats the input as const.
  stream_.next_in = const_cast<uint8_t*>(input.data());
  // Enqueueing may run a page's script, which may change the input, so all of
  // the output comes first.
  std::vector<v8::Local<v8::Uint8Array>> buffers;
  do {
    stream_.avail_out = static_cast<uInt>(out_buffer_.size());
    stream_.next_out = out_buffer_.data();
    const int err = inflate(&stream_, finished ? Z_FINISH : Z_NO_FLUSH);
    if (err != Z_OK && err != Z_STREAM_END && err != Z_BUF_ERROR) {
      EnqueueBuffers(buffers, controller, exception_state);
      if (exception_state.HadException()) {
        return;
      }
      if (err == Z_DATA_ERROR) {
        exception_state.ThrowTypeError(base::StrCat(
            {"The compressed data was not valid: ",
             stream_.msg ? stream_.msg : "", "."}));
      } else {
        exception_state.ThrowTypeError("The compressed data was not valid.");
      }
      return;
    }
    const size_t bytes = out_buffer_.size() - stream_.avail_out;
    if (bytes) {
      buffers.push_back(
          NewUint8Array(isolate, base::span(out_buffer_).first(bytes)));
    }
    if (err == Z_STREAM_END) {
      reached_end_ = true;
      const bool junk_found = stream_.avail_in > 0;
      EnqueueBuffers(buffers, controller, exception_state);
      if (exception_state.HadException()) {
        return;
      }
      if (junk_found) {
        exception_state.ThrowTypeError(
            "Junk found after end of compressed data.");
      }
      return;
    }
  } while (stream_.avail_out == 0);
  CHECK_EQ(stream_.avail_in, 0u);
  EnqueueBuffers(buffers, controller, exception_state);
}

}  // namespace mitto
