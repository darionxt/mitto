#include "mitto/dom/text_decoder_transformer.h"

#include <stddef.h>
#include <stdint.h>

#include <utility>

#include "base/check.h"
#include "base/containers/span.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_promise.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/dom/stream_chunks.h"
#include "mitto/dom/transform_stream_default_controller.h"
#include "mitto/encoding/text_codec.h"
#include "mitto/encoding/text_encoding_registry.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

namespace {

// WTF's kStringMaxUCharLength, as in text_decoder.cc.
constexpr size_t kStringMaxUCharLength =
    ((size_t{1} << 31) - (size_t{1} << 21) - 12) / sizeof(char16_t);

bool EncodingHasBomRemoval(const std::string& encoding) {
  return encoding == "UTF-8" || encoding == "UTF-16LE" ||
         encoding == "UTF-16BE";
}

}  // namespace

TextDecoderTransformer::TextDecoderTransformer(const std::string& encoding,
                                               bool fatal,
                                               bool ignore_bom)
    : decoder_(NewTextCodec(encoding)),
      fatal_(fatal),
      ignore_bom_(ignore_bom),
      encoding_has_bom_removal_(EncodingHasBomRemoval(encoding)) {
  CHECK(decoder_);
}

TextDecoderTransformer::~TextDecoderTransformer() = default;

v8::Local<v8::Promise> TextDecoderTransformer::Transform(
    v8::Local<v8::Value> chunk,
    TransformStreamDefaultController* controller,
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  base::span<const uint8_t> bytes;
  if (!BytesOfBufferSource(isolate, chunk, exception_state, &bytes)) {
    return {};
  }
  // Room for up to 3 bytes of a sequence that UTF-8 keeps from before.
  if (bytes.size() > kStringMaxUCharLength - 3) {
    exception_state.ThrowRangeError(
        "Buffer size exceeds maximum heap object size.");
    return {};
  }
  bool saw_error = false;
  std::u16string output =
      decoder_->Decode(bytes, FlushBehavior::kDoNotFlush, fatal_, saw_error);
  if (fatal_ && saw_error) {
    exception_state.ThrowTypeError("The encoded data is not valid.");
    return {};
  }
  return EnqueueOutput(std::move(output), controller, exception_state);
}

v8::Local<v8::Promise> TextDecoderTransformer::Flush(
    TransformStreamDefaultController* controller,
    bindings::ExceptionState& exception_state) {
  bool saw_error = false;
  std::u16string output =
      decoder_->Decode({}, FlushBehavior::kDataEof, fatal_, saw_error);
  if (fatal_ && saw_error) {
    exception_state.ThrowTypeError("The encoded data is not valid.");
    return {};
  }
  return EnqueueOutput(std::move(output), controller, exception_state);
}

v8::Local<v8::Promise> TextDecoderTransformer::EnqueueOutput(
    std::u16string output,
    TransformStreamDefaultController* controller,
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  if (output.empty()) {
    return bindings::ResolvedUndefinedPromise(isolate);
  }
  if (!ignore_bom_ && !bom_seen_) {
    bom_seen_ = true;
    if (encoding_has_bom_removal_ && output[0] == 0xFEFF) {
      output.erase(0, 1);
      if (output.empty()) {
        return bindings::ResolvedUndefinedPromise(isolate);
      }
    }
  }
  controller->Enqueue(bindings::ToV8String(isolate, output), exception_state);
  return bindings::ResolvedUndefinedPromise(isolate);
}

}  // namespace mitto
