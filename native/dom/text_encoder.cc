#include "mitto/dom/text_encoder.h"

#include <stdint.h>

#include <string>
#include <tuple>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_text_encoder.h"
#include "mitto/encoding/text_codec_utf8.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

namespace {

// The bytes that `view` sees, which stay where they are while no script runs.
base::span<uint8_t> BytesOf(v8::Local<v8::ArrayBufferView> view) {
  // Asking for the buffer moves the bytes of a view that V8 keeps on its heap
  // off it, which a page cannot see.
  const v8::Local<v8::ArrayBuffer> buffer = view->Buffer();
  const size_t length = view->ByteLength();
  if (!length) {
    return {};
  }
  // SAFETY: V8 gives the buffer's data and its length in bytes, and a view's
  // bytes lie within its buffer.
  return UNSAFE_BUFFERS(base::span(static_cast<uint8_t*>(buffer->Data()),
                                   buffer->ByteLength()))
      .subspan(view->ByteOffset(), length);
}

}  // namespace

// static
TextEncoder* TextEncoder::Create(bindings::ExceptionState&) {
  return bindings::MakeGarbageCollected<TextEncoder>();
}

TextEncoder::TextEncoder() = default;
TextEncoder::~TextEncoder() = default;

const bindings::WrapperTypeInfo* TextEncoder::GetWrapperTypeInfo() const {
  return &bindings::V8TextEncoder::kWrapperTypeInfo;
}

std::u16string TextEncoder::Encoding() const {
  return u"utf-8";
}

v8::Local<v8::Uint8Array> TextEncoder::Encode(
    const std::u16string& input,
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const std::string bytes = EncodeUtf8(input);
  // Blink's kThrowExceptionWhenTextEncodeOOM, on by default: a buffer that
  // cannot be had throws.
  v8::Local<v8::ArrayBuffer> buffer;
  if (!v8::ArrayBuffer::MaybeNew(isolate, bytes.size()).ToLocal(&buffer)) {
    exception_state.ThrowDOMException("UnknownError",
                                      "Failed to allocate buffer.");
    return v8::Local<v8::Uint8Array>();
  }
  if (!bytes.empty()) {
    // SAFETY: the new buffer holds bytes.size() bytes.
    UNSAFE_BUFFERS(
        base::span(static_cast<uint8_t*>(buffer->Data()), bytes.size()))
        .copy_from(base::as_byte_span(bytes));
  }
  return v8::Uint8Array::New(buffer, 0, bytes.size());
}

v8::Local<v8::Object> TextEncoder::EncodeInto(
    const std::u16string& source,
    v8::Local<v8::Uint8Array> destination) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const Utf8EncodeIntoResult result =
      EncodeUtf8Into(source, BytesOf(destination));
  // TextEncoderEncodeIntoResult, as Blink's ToV8 of a dictionary writes it.
  const v8::Local<v8::Object> object = v8::Object::New(isolate);
  std::ignore = object->CreateDataProperty(
      context, v8::String::NewFromUtf8Literal(isolate, "read"),
      v8::Number::New(isolate, static_cast<double>(result.code_units_read)));
  std::ignore = object->CreateDataProperty(
      context, v8::String::NewFromUtf8Literal(isolate, "written"),
      v8::Number::New(isolate, static_cast<double>(result.bytes_written)));
  return object;
}

}  // namespace mitto
