#include "mitto/dom/deflate_transformer.h"

#include <stdint.h>

#include <limits>
#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "base/containers/span.h"
#include "base/notreached.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_promise.h"
#include "mitto/dom/compression_format.h"
#include "mitto/dom/stream_chunks.h"
#include "mitto/dom/transform_stream_default_controller.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-typed-array.h"

namespace mitto {

DeflateTransformer::DeflateTransformer(CompressionFormat format, int level)
    : out_buffer_(kBufferSize), stream_{}, gzip_header_{} {
  CHECK(level >= 1 && level <= 9);
  constexpr int kWindowBits = 15;
  constexpr int kUseGzip = 16;
  int err = Z_OK;
  switch (format) {
    case CompressionFormat::kDeflate:
      err = deflateInit2(&stream_, level, Z_DEFLATED, kWindowBits, 8,
                         Z_DEFAULT_STRATEGY);
      break;
    case CompressionFormat::kGzip:
      err = deflateInit2(&stream_, level, Z_DEFLATED, kWindowBits + kUseGzip, 8,
                         Z_DEFAULT_STRATEGY);
      break;
    case CompressionFormat::kDeflateRaw:
      err = deflateInit2(&stream_, level, Z_DEFLATED, -kWindowBits, 8,
                         Z_DEFAULT_STRATEGY);
      break;
  }
  CHECK_EQ(Z_OK, err);
  if (format == CompressionFormat::kGzip) {
    // zlib writes the operating system that it was built for in a gzip
    // header: 10 on Windows, 19 on macOS and 3 on Linux. A Page presents
    // Windows Chrome, so its header says Windows on every Host, as Windows
    // Chrome's does, with every other field as zlib's default header has it.
    constexpr int kWindowsOsCode = 10;
    gzip_header_.os = kWindowsOsCode;
    CHECK_EQ(Z_OK, deflateSetHeader(&stream_, &gzip_header_));
  }
}

DeflateTransformer::~DeflateTransformer() {
  if (!was_flush_called_) {
    deflateEnd(&stream_);
  }
}

v8::Local<v8::Promise> DeflateTransformer::Transform(
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
  Deflate(bytes, /*finished=*/false, controller, exception_state);
  return bindings::ResolvedUndefinedPromise(isolate);
}

v8::Local<v8::Promise> DeflateTransformer::Flush(
    TransformStreamDefaultController* controller,
    bindings::ExceptionState& exception_state) {
  Deflate({}, /*finished=*/true, controller, exception_state);
  was_flush_called_ = true;
  deflateEnd(&stream_);
  out_buffer_.clear();
  return bindings::ResolvedUndefinedPromise(exception_state.GetIsolate());
}

void DeflateTransformer::Deflate(base::span<const uint8_t> input,
                                 bool finished,
                                 TransformStreamDefaultController* controller,
                                 bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  stream_.avail_in = static_cast<uInt>(input.size());
  // zlib treats the input as const.
  stream_.next_in = const_cast<uint8_t*>(input.data());
  // Enqueueing may run a page's script, which may change the input, so all of
  // the output comes first.
  std::vector<v8::Local<v8::Uint8Array>> buffers;
  do {
    stream_.avail_out = static_cast<uInt>(out_buffer_.size());
    stream_.next_out = out_buffer_.data();
    const int err = deflate(&stream_, finished ? Z_FINISH : Z_NO_FLUSH);
    CHECK((finished && err == Z_STREAM_END) || err == Z_OK ||
          err == Z_BUF_ERROR);
    const size_t bytes = out_buffer_.size() - stream_.avail_out;
    if (bytes) {
      buffers.push_back(
          NewUint8Array(isolate, base::span(out_buffer_).first(bytes)));
    }
  } while (stream_.avail_out == 0);
  CHECK_EQ(stream_.avail_in, 0u);
  for (const v8::Local<v8::Uint8Array> buffer : buffers) {
    controller->Enqueue(buffer, exception_state);
    if (exception_state.HadException()) {
      return;
    }
  }
}

}  // namespace mitto
