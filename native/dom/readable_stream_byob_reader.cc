#include "mitto/dom/readable_stream_byob_reader.h"

#include <stddef.h>

#include <tuple>

#include "base/check.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_promise.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_readable_stream_byob_reader.h"
#include "mitto/dom/miscellaneous_operations.h"
#include "mitto/dom/read_request.h"
#include "mitto/dom/readable_byte_stream_controller.h"
#include "mitto/dom/readable_stream.h"
#include "mitto/dom/readable_stream_controller.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

namespace {

// A ReadableStreamReadResult as Blink's bindings make the dictionary: its
// members in the order of their names, `done` then `value`.
v8::Local<v8::Object> CreateBYOBReadResult(v8::Isolate* isolate,
                                           v8::Local<v8::Value> value,
                                           bool done) {
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::Local<v8::Object> result = v8::Object::New(isolate);
  std::ignore = result->CreateDataProperty(
      context, v8::String::NewFromUtf8Literal(isolate, "done"),
      v8::Boolean::New(isolate, done));
  std::ignore = result->CreateDataProperty(
      context, v8::String::NewFromUtf8Literal(isolate, "value"), value);
  return result;
}

}  // namespace

class ReadableStreamBYOBReader::BYOBReaderReadIntoRequest final
    : public ReadIntoRequest {
 public:
  explicit BYOBReaderReadIntoRequest(bindings::ScriptPromiseResolver* resolver)
      : resolver_(resolver) {}

  void ChunkSteps(v8::Isolate* isolate,
                  v8::Local<v8::ArrayBufferView> chunk,
                  bindings::ExceptionState&) const override {
    // Resolve promise with «[ "value" → chunk, "done" → false ]».
    resolver_->Resolve(isolate, CreateBYOBReadResult(isolate, chunk, false));
  }

  void CloseSteps(v8::Isolate* isolate,
                  v8::Local<v8::ArrayBufferView> chunk) const override {
    // Resolve promise with «[ "value" → chunk, "done" → true ]».
    const v8::Local<v8::Value> value =
        chunk.IsEmpty() ? v8::Local<v8::Value>(v8::Undefined(isolate))
                        : v8::Local<v8::Value>(chunk);
    resolver_->Resolve(isolate, CreateBYOBReadResult(isolate, value, true));
  }

  void ErrorSteps(v8::Isolate* isolate, v8::Local<v8::Value> e) const override {
    // Reject promise with e.
    resolver_->Reject(isolate, e);
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(resolver_);
    ReadIntoRequest::Trace(visitor);
  }

 private:
  const cppgc::Member<bindings::ScriptPromiseResolver> resolver_;
};

// static
ReadableStreamBYOBReader* ReadableStreamBYOBReader::Create(
    ReadableStream* stream,
    bindings::ExceptionState& exception_state) {
  auto* const reader =
      bindings::MakeGarbageCollected<ReadableStreamBYOBReader>();
  SetUpBYOBReader(v8::Isolate::GetCurrent(), reader, stream, exception_state);
  if (exception_state.HadException()) {
    return nullptr;
  }
  return reader;
}

ReadableStreamBYOBReader::ReadableStreamBYOBReader() = default;

ReadableStreamBYOBReader::~ReadableStreamBYOBReader() = default;

bool ReadableStreamBYOBReader::IsDefaultReader() const {
  return false;
}

bool ReadableStreamBYOBReader::IsBYOBReader() const {
  return true;
}

const bindings::WrapperTypeInfo* ReadableStreamBYOBReader::GetWrapperTypeInfo()
    const {
  return &bindings::V8ReadableStreamBYOBReader::kWrapperTypeInfo;
}

void ReadableStreamBYOBReader::Trace(cppgc::Visitor* visitor) const {
  read_into_requests_.Trace(visitor);
  ReadableStreamGenericReader::Trace(visitor);
}

v8::Local<v8::Promise> ReadableStreamBYOBReader::Read(
    v8::Local<v8::ArrayBufferView> view,
    const bindings::ReadableStreamBYOBReaderReadOptions& options,
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  // https://streams.spec.whatwg.org/#byob-reader-read
  // 1. If view.[[ByteLength]] is 0, return a promise rejected with a TypeError
  // exception.
  if (view->ByteLength() == 0) {
    exception_state.ThrowTypeError(
        "This readable stream reader cannot be used to read as the view has "
        "byte length equal to 0");
    return {};
  }

  // 2. If view.[[ViewedArrayBuffer]].[[ArrayBufferByteLength]] is 0, return a
  // promise rejected with a TypeError exception.
  if (view->Buffer()->ByteLength() == 0) {
    exception_state.ThrowTypeError(
        "This readable stream reader cannot be used to read as the viewed "
        "array buffer has 0 byte length");
    return {};
  }

  // 3. If ! IsDetachedBuffer(view.[[ViewedArrayBuffer]]) is true, return a
  // promise rejected with a TypeError exception.
  if (view->Buffer()->WasDetached()) {
    exception_state.ThrowTypeError(
        "This readable stream reader cannot be used to read as the viewed "
        "array buffer is detached");
    return {};
  }

  // 4. If options["min"] is 0, return a promise rejected with a TypeError
  // exception.
  if (options.min == 0) {
    exception_state.ThrowTypeError("min cannot be 0");
    return {};
  }

  if (!view->IsDataView()) {
    // 5. If view has a [[TypedArrayName]] internal slot,
    //   1. If options["min"] > view.[[ArrayLength]], return a promise
    //      rejected with a RangeError exception.
    const size_t array_length = view.As<v8::TypedArray>()->Length();
    if (options.min > array_length) {
      exception_state.ThrowRangeError(
          "min cannot be larger than view's length");
      return {};
    }
  } else {
    // 6. Otherwise (i.e., it is a DataView),
    //   1. If options["min"] > view.[[ByteLength]], return a promise
    //      rejected with a RangeError exception.
    if (options.min > view->ByteLength()) {
      exception_state.ThrowRangeError(
          "min cannot be larger than view's byteLength");
      return {};
    }
  }
  const uint64_t min = options.min;

  // 7. If this.[[stream]] is undefined, return a promise rejected with a
  // TypeError exception.
  if (!owner_readable_stream_) {
    exception_state.ThrowTypeError(
        "This readable stream reader has been released and cannot be used to "
        "read from its previous owner stream");
    return {};
  }

  // 8. Let promise be a new promise.
  auto* const resolver =
      bindings::MakeGarbageCollected<bindings::ScriptPromiseResolver>(isolate);

  // 9. Let readIntoRequest be a new read-into request with the following items:
  //    chunk steps, given chunk
  //      1. Resolve promise with «[ "value" → chunk, "done" → false ]».
  //    close steps, given chunk
  //      1. Resolve promise with «[ "value" → chunk, "done" → true ]».
  //    error steps, given e
  //      1. Reject promise with e.
  auto* const read_into_request =
      bindings::MakeGarbageCollected<BYOBReaderReadIntoRequest>(resolver);

  // 10. Perform ! ReadableStreamBYOBReaderRead(this, view, min,
  //     readIntoRequest).
  Read(isolate, this, view, min, read_into_request, exception_state);

  // 11. Return promise.
  return resolver->Promise(isolate);
}

// static
void ReadableStreamBYOBReader::Read(v8::Isolate* isolate,
                                    ReadableStreamBYOBReader* reader,
                                    v8::Local<v8::ArrayBufferView> view,
                                    uint64_t min,
                                    ReadIntoRequest* read_into_request,
                                    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#readable-stream-byob-reader-read
  // 1. Let stream be reader.[[stream]].
  ReadableStream* const stream = reader->owner_readable_stream_;

  // 2. Assert: stream is not undefined.
  CHECK(stream);

  // 3. Set stream.[[disturbed]] to true.
  stream->is_disturbed_ = true;

  // 4. If stream.[[state]] is "errored", perform readIntoRequest's error steps
  // given stream.[[storedError]].
  if (stream->state_ == ReadableStream::kErrored) {
    read_into_request->ErrorSteps(isolate, stream->GetStoredError(isolate));
  } else {
    // 5. Otherwise, perform !
    // ReadableByteStreamControllerPullInto(stream.[[controller]], view, min,
    // readIntoRequest).
    ReadableStreamController* const controller =
        stream->readable_stream_controller_;
    CHECK(controller->IsByteStreamController());
    ReadableByteStreamController::PullInto(
        isolate, static_cast<ReadableByteStreamController*>(controller), view,
        min, read_into_request, exception_state);
  }
}

// static
void ReadableStreamBYOBReader::ErrorReadIntoRequests(
    v8::Isolate* isolate,
    ReadableStreamBYOBReader* reader,
    v8::Local<v8::Value> e) {
  // https://streams.spec.whatwg.org/#abstract-opdef-readablestreambyobreadererrorreadintorequests
  // 1. Let readIntoRequests be reader.[[readIntoRequests]].
  // 2. Set reader.[[readIntoRequests]] to a new empty list.
  HeapDeque<ReadIntoRequest> read_into_requests;
  read_into_requests.swap(reader->read_into_requests_);
  // 3. For each readIntoRequest of readIntoRequests,
  for (size_t i = 0; i < read_into_requests.size(); ++i) {
    //   a. Perform readIntoRequest’s error steps, given e.
    read_into_requests[i]->ErrorSteps(isolate, e);
  }
}

// static
void ReadableStreamBYOBReader::Release(v8::Isolate* isolate,
                                       ReadableStreamBYOBReader* reader) {
  // https://streams.spec.whatwg.org/#abstract-opdef-readablestreambyobreaderrelease
  // 1. Perform ! ReadableStreamReaderGenericRelease(reader).
  ReadableStreamGenericReader::GenericRelease(isolate, reader);

  // 2. Let e be a new TypeError exception.
  const v8::Local<v8::Value> e =
      CreateTypeError(isolate, "Releasing BYOB reader");

  // 3. Perform ! ReadableStreamBYOBReaderErrorReadIntoRequests(reader, e).
  ErrorReadIntoRequests(isolate, reader, e);
}

void ReadableStreamBYOBReader::ReleaseLock(
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#byob-reader-release-lock
  // 1. If this.[[stream]] is undefined, return.
  if (!owner_readable_stream_) {
    return;
  }

  // 2. Perform ! ReadableStreamBYOBReaderRelease(this).
  Release(exception_state.GetIsolate(), this);
}

// static
void ReadableStreamBYOBReader::SetUpBYOBReader(
    v8::Isolate* isolate,
    ReadableStreamBYOBReader* reader,
    ReadableStream* stream,
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#set-up-readable-stream-byob-reader
  // 1. If ! IsReadableStreamLocked(stream) is true, throw a TypeError
  // exception.
  if (ReadableStream::IsLocked(stream)) {
    exception_state.ThrowTypeError(
        "ReadableStreamBYOBReader constructor can only accept readable streams "
        "that are not yet locked to a reader");
    return;
  }

  // 2. If stream.[[controller]] does not implement
  // ReadableByteStreamController, throw a TypeError exception.
  if (!stream->readable_stream_controller_->IsByteStreamController()) {
    exception_state.ThrowTypeError(
        "Cannot use a BYOB reader with a non-byte stream");
    return;
  }

  // 3. Perform ! ReadableStreamReaderGenericInitialize(reader, stream).
  ReadableStreamGenericReader::GenericInitialize(isolate, reader, stream);

  // 4. Set reader.[[readIntoRequests]] to a new empty list.
  CHECK(reader->read_into_requests_.empty());
}

}  // namespace mitto
