// The transformer of a CompressionStream, as Blink's: it compresses each
// chunk with zlib's deflate, at Blink's level 6.
//
// Upstream: third_party/blink/renderer/modules/compression/deflate_transformer.h
// Upstream: third_party/blink/renderer/modules/compression/deflate_transformer.cc

#ifndef MITTO_DOM_DEFLATE_TRANSFORMER_H_
#define MITTO_DOM_DEFLATE_TRANSFORMER_H_

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "base/containers/span.h"
#include "mitto/dom/transform_stream_transformer.h"
#include "third_party/zlib/zlib.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-promise.h"

namespace mitto {

enum class CompressionFormat;

class DeflateTransformer final : public TransformStreamTransformer {
 public:
  DeflateTransformer(CompressionFormat format, int level);
  DeflateTransformer(const DeflateTransformer&) = delete;
  DeflateTransformer& operator=(const DeflateTransformer&) = delete;
  ~DeflateTransformer() override;

  v8::Local<v8::Promise> Transform(
      v8::Local<v8::Value> chunk,
      TransformStreamDefaultController* controller,
      bindings::ExceptionState& exception_state) override;
  v8::Local<v8::Promise> Flush(
      TransformStreamDefaultController* controller,
      bindings::ExceptionState& exception_state) override;

 private:
  void Deflate(base::span<const uint8_t> input,
               bool finished,
               TransformStreamDefaultController* controller,
               bindings::ExceptionState& exception_state);

  std::vector<uint8_t> out_buffer_;
  z_stream stream_;
  // The gzip header that the stream writes, which zlib reads until it has.
  gz_header gzip_header_;
  bool was_flush_called_ = false;

  // The size that Blink found best.
  static constexpr size_t kBufferSize = 16384;
};

}  // namespace mitto

#endif  // MITTO_DOM_DEFLATE_TRANSFORMER_H_
