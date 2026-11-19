#include "mitto/dom/readable_stream_generic_reader.h"

#include "base/check.h"
#include "base/check_op.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_promise.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/miscellaneous_operations.h"
#include "mitto/dom/readable_stream.h"
#include "mitto/dom/readable_stream_controller.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

ReadableStreamGenericReader::ReadableStreamGenericReader() = default;

ReadableStreamGenericReader::~ReadableStreamGenericReader() = default;

void ReadableStreamGenericReader::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(closed_resolver_);
  visitor->Trace(owner_readable_stream_);
  bindings::ScriptWrappable::Trace(visitor);
}

v8::Local<v8::Promise> ReadableStreamGenericReader::Closed() const {
  // Step 1. Return this.[[closedPromise]].
  return closed_resolver_->Promise(v8::Isolate::GetCurrent());
}

v8::Local<v8::Promise> ReadableStreamGenericReader::Cancel(
    const std::optional<v8::Local<v8::Value>>& reason,
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  // Step 1. If this.[[stream]] is undefined, return a promise rejected with a
  // TypeError exception.
  if (!owner_readable_stream_) {
    exception_state.ThrowTypeError(
        "This readable stream reader has been released and cannot be used to "
        "cancel its previous owner stream");
    return {};
  }
  // Step 2. Return ! ReadableStreamReaderGenericCancel(this, reason).
  return GenericCancel(isolate, this, reason.value_or(v8::Undefined(isolate)));
}

// static
void ReadableStreamGenericReader::GenericRelease(
    v8::Isolate* isolate,
    ReadableStreamGenericReader* reader) {
  // Step 1. Let stream be reader.[[stream]].
  ReadableStream* const stream = reader->owner_readable_stream_;
  // Steps 2-3. Assert: stream is not undefined, and stream.[[reader]] is
  // reader.
  CHECK(stream);
  CHECK_EQ(stream->reader_, reader);
  // Step 4. If stream.[[state]] is "readable", reject reader.[[closedPromise]]
  // with a TypeError exception.
  // Step 5. Otherwise, set reader.[[closedPromise]] to a promise rejected with
  // a TypeError exception.
  if (stream->state_ != ReadableStream::kReadable) {
    reader->closed_resolver_ =
        bindings::MakeGarbageCollected<bindings::ScriptPromiseResolver>(isolate);
  }
  reader->closed_resolver_->Promise(isolate)->MarkAsSilent();
  reader->closed_resolver_->Reject(
      isolate, CreateTypeError(isolate,
                               "This readable stream reader has been released "
                               "and cannot be used to monitor the stream's "
                               "state"));
  // Step 6. Set reader.[[closedPromise]].[[PromiseIsHandled]] to true.
  reader->closed_resolver_->Promise(isolate)->MarkAsHandled();
  // Step 7. Perform ! stream.[[controller]].[[ReleaseSteps]]().
  stream->readable_stream_controller_->ReleaseSteps();
  // Step 8. Set stream.[[reader]] to undefined.
  stream->reader_ = nullptr;
  // Step 9. Set reader.[[stream]] to undefined.
  reader->owner_readable_stream_ = nullptr;
}

// static
v8::Local<v8::Promise> ReadableStreamGenericReader::GenericCancel(
    v8::Isolate* isolate,
    ReadableStreamGenericReader* reader,
    v8::Local<v8::Value> reason) {
  // Steps 1-2. Let stream be reader.[[stream]], which is not undefined.
  ReadableStream* const stream = reader->owner_readable_stream_;
  CHECK(stream);
  // Step 3. Return ! ReadableStreamCancel(stream, reason).
  return ReadableStream::Cancel(isolate, stream, reason);
}

// static
void ReadableStreamGenericReader::GenericInitialize(
    v8::Isolate* isolate,
    ReadableStreamGenericReader* reader,
    ReadableStream* stream) {
  // Step 1. Set reader.[[stream]] to stream.
  reader->owner_readable_stream_ = stream;
  // Step 2. Set stream.[[reader]] to reader.
  stream->reader_ = reader;
  reader->closed_resolver_ =
      bindings::MakeGarbageCollected<bindings::ScriptPromiseResolver>(isolate);
  switch (stream->state_) {
    case ReadableStream::kReadable:
      // Step 3. If stream.[[state]] is "readable", set
      // reader.[[closedPromise]] to a new promise.
      break;
    case ReadableStream::kClosed:
      // Step 4. Otherwise, if stream.[[state]] is "closed", set
      // reader.[[closedPromise]] to a promise resolved with undefined.
      reader->closed_resolver_->Resolve(isolate);
      break;
    case ReadableStream::kErrored:
      // Step 5. Otherwise, set reader.[[closedPromise]] to a promise rejected
      // with stream.[[storedError]], and set its [[PromiseIsHandled]] to true.
      reader->closed_resolver_->Promise(isolate)->MarkAsSilent();
      reader->closed_resolver_->Reject(isolate, stream->GetStoredError(isolate));
      reader->closed_resolver_->Promise(isolate)->MarkAsHandled();
      break;
  }
}

}  // namespace mitto
