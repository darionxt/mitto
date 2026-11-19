#include "mitto/dom/text_encoder_stream.h"

#include <stdint.h>

#include <optional>
#include <string>
#include <string_view>

#include "base/check_op.h"
#include "base/containers/span.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_promise.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_text_encoder_stream.h"
#include "mitto/dom/stream_chunks.h"
#include "mitto/dom/transform_stream.h"
#include "mitto/dom/transform_stream_default_controller.h"
#include "mitto/dom/transform_stream_transformer.h"
#include "mitto/encoding/text_codec_utf8.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

namespace {

constexpr std::string_view kReplacementCharacterInUtf8 = "\xEF\xBF\xBD";

bool IsLeadingSurrogate(char16_t c) {
  return c >= 0xD800 && c <= 0xDBFF;
}

bool IsTrailingSurrogate(char16_t c) {
  return c >= 0xDC00 && c <= 0xDFFF;
}

}  // namespace

class TextEncoderStream::Transformer final : public TransformStreamTransformer {
 public:
  Transformer() = default;
  Transformer(const Transformer&) = delete;
  Transformer& operator=(const Transformer&) = delete;
  ~Transformer() override = default;

  // The "encode and enqueue a chunk" algorithm. Only the characters at the
  // ends of chunks are special.
  v8::Local<v8::Promise> Transform(
      v8::Local<v8::Value> chunk,
      TransformStreamDefaultController* controller,
      bindings::ExceptionState& exception_state) override {
    v8::Isolate* const isolate = exception_state.GetIsolate();
    std::u16string input;
    {
      const bindings::TryRethrowScope rethrow_scope(isolate, exception_state);
      if (!bindings::FromV8String(isolate, chunk, &input)) {
        return {};
      }
    }
    if (input.empty()) {
      return bindings::ResolvedUndefinedPromise(isolate);
    }
    const std::optional<char16_t> high_surrogate = pending_high_surrogate_;
    pending_high_surrogate_ = std::nullopt;
    std::u16string_view rest = input;
    std::string prefix;
    if (high_surrogate.has_value()) {
      if (IsTrailingSurrogate(rest.front())) {
        const char16_t astral_character[] = {*high_surrogate, rest.front()};
        prefix = EncodeUtf8(std::u16string_view(astral_character, 2));
        rest.remove_prefix(1);
      } else {
        prefix = kReplacementCharacterInUtf8;
      }
    }
    if (!rest.empty() && IsLeadingSurrogate(rest.back())) {
      pending_high_surrogate_ = rest.back();
      rest.remove_suffix(1);
    }
    const std::string result = prefix + EncodeUtf8(rest);
    if (result.empty()) {
      return bindings::ResolvedUndefinedPromise(isolate);
    }
    controller->Enqueue(NewUint8Array(isolate, base::as_byte_span(result)),
                        exception_state);
    return bindings::ResolvedUndefinedPromise(isolate);
  }

  // The "encode and flush" algorithm.
  v8::Local<v8::Promise> Flush(
      TransformStreamDefaultController* controller,
      bindings::ExceptionState& exception_state) override {
    v8::Isolate* const isolate = exception_state.GetIsolate();
    if (!pending_high_surrogate_.has_value()) {
      return bindings::ResolvedUndefinedPromise(isolate);
    }
    controller->Enqueue(
        NewUint8Array(isolate, base::as_byte_span(kReplacementCharacterInUtf8)),
        exception_state);
    return bindings::ResolvedUndefinedPromise(isolate);
  }

 private:
  std::optional<char16_t> pending_high_surrogate_;
};

// static
TextEncoderStream* TextEncoderStream::Create(
    bindings::ExceptionState& exception_state) {
  TransformStream* const transform = TransformStream::Create(
      exception_state.GetIsolate(),
      bindings::MakeGarbageCollected<Transformer>(), exception_state);
  if (exception_state.HadException()) {
    return nullptr;
  }
  return bindings::MakeGarbageCollected<TextEncoderStream>(transform);
}

TextEncoderStream::TextEncoderStream(TransformStream* transform)
    : transform_(transform) {}

TextEncoderStream::~TextEncoderStream() = default;

const bindings::WrapperTypeInfo* TextEncoderStream::GetWrapperTypeInfo()
    const {
  return &bindings::V8TextEncoderStream::kWrapperTypeInfo;
}

void TextEncoderStream::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(transform_);
  bindings::ScriptWrappable::Trace(visitor);
}

std::u16string TextEncoderStream::Encoding() const {
  return u"utf-8";
}

ReadableStream* TextEncoderStream::Readable() const {
  return transform_->Readable();
}

WritableStream* TextEncoderStream::Writable() const {
  return transform_->Writable();
}

}  // namespace mitto
