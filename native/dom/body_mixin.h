// The Fetch Standard's Body interface mixin, which Response and Request
// include, as Blink's Body: a body's stream, bodyUsed, and the methods that
// read all of a body, which wait for it through a FetchDataLoader and settle
// from a later networking task, as Blink's ResolveLater() does. Blink's class
// is Body, which a member named body() would hide here.
//
// Upstream: third_party/blink/renderer/core/fetch/body.h
// Upstream: third_party/blink/renderer/core/fetch/body.cc

#ifndef MITTO_DOM_BODY_MIXIN_H_
#define MITTO_DOM_BODY_MIXIN_H_

#include <string>

#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-promise.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class BodyStreamBuffer;
class ReadableStream;

class BodyMixin {
 public:
  BodyMixin(const BodyMixin&) = delete;
  BodyMixin& operator=(const BodyMixin&) = delete;

  // The members of the interface mixin.
  bool BodyUsed() const { return IsBodyUsed(); }
  v8::Local<v8::Promise> ArrayBuffer(bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> Blob(bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> Bytes(bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> FormData(bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> Json(bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> Text(bindings::ExceptionState& exception_state);
  ReadableStream* TextStream(bindings::ExceptionState& exception_state);
  // The `body` attribute of each interface that includes it.
  ReadableStream* BodyStream() const;

  // Whether its body's stream is disturbed, and whether it is locked.
  virtual bool IsBodyUsed() const;
  bool IsBodyLocked() const;

 protected:
  BodyMixin() = default;
  virtual ~BodyMixin() = default;

  // Its body, or null for a null body.
  virtual BodyStreamBuffer* BodyBuffer() const = 0;
  // Its Content-Type, its values joined, or empty without one, as Blink's
  // ContentType() reads it from the header list.
  virtual std::string ContentType() const = 0;

 private:
  enum class ReadAs {
    kArrayBuffer,
    kBlob,
    kBytes,
    kFormData,
    kFailure,
    kJson,
    // Rejects without reading the body, as formData() of a multipart type
    // without a boundary.
    kNothing,
    kText,
    kUrlEncodedFormData,
  };

  // The type of a Blob that blob() makes: its Content-Type, in lowercase, as
  // Blink's FetchHeaderList::ExtractMIMEType() gives it.
  std::string MimeType() const;

  void RejectInvalidConsumption(
      bindings::ExceptionState& exception_state) const;
  // `boundary` is that of a multipart/form-data body, for kFormData.
  v8::Local<v8::Promise> LoadAndConvertBody(
      ReadAs read_as,
      bindings::ExceptionState& exception_state,
      std::string boundary = std::string());
};

}  // namespace mitto

#endif  // MITTO_DOM_BODY_MIXIN_H_
