// The transformer of a DecompressionStream, as Blink's: it decompresses each
// chunk with zlib's inflate, and refuses data after the end and a truncated
// input.
//
// Upstream: third_party/blink/renderer/modules/compression/inflate_transformer.h
// Upstream: third_party/blink/renderer/modules/compression/inflate_transformer.cc

#ifndef MITTO_DOM_INFLATE_TRANSFORMER_H_
#define MITTO_DOM_INFLATE_TRANSFORMER_H_

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

class InflateTransformer final : public TransformStreamTransformer {
 public:
  explicit InflateTransformer(CompressionFormat format);
  InflateTransformer(const InflateTransformer&) = delete;
  InflateTransformer& operator=(const InflateTransformer&) = delete;
  ~InflateTransformer() override;

  v8::Local<v8::Promise> Transform(
      v8::Local<v8::Value> chunk,
      TransformStreamDefaultController* controller,
      bindings::ExceptionState& exception_state) override;
  v8::Local<v8::Promise> Flush(
      TransformStreamDefaultController* controller,
      bindings::ExceptionState& exception_state) override;

 private:
  void Inflate(base::span<const uint8_t> input,
               bool finished,
               TransformStreamDefaultController* controller,
               bindings::ExceptionState& exception_state);

  std::vector<uint8_t> out_buffer_;
  z_stream stream_;
  bool was_flush_called_ = false;
  bool reached_end_ = false;
  // The size that Blink found best.
  static constexpr size_t kBufferSize = 16384;
};

}  // namespace mitto

#endif  // MITTO_DOM_INFLATE_TRANSFORMER_H_
