#include "mitto/dom/readable_stream_default_reader.h"

#include <tuple>

#include "base/check.h"
#include "base/check_op.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_promise.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_readable_stream_default_reader.h"
#include "mitto/dom/miscellaneous_operations.h"
#include "mitto/dom/read_request.h"
#include "mitto/dom/readable_stream.h"
#include "mitto/dom/readable_stream_controller.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

namespace {

// A ReadableStreamReadResult as Blink's bindings make the dictionary: its
// members in the order of their names, `done` then `value`.
v8::Local<v8::Object> ReadResult(v8::Isolate* isolate,
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

class ReadableStreamDefaultReader::DefaultReaderReadRequest final
    : public ReadRequest {
 public:
  explicit DefaultReaderReadRequest(bindings::ScriptPromiseResolver* resolver)
      : resolver_(resolver) {}

  void ChunkSteps(v8::Isolate* isolate,
                  v8::Local<v8::Value> chunk,
                  bindings::ExceptionState&) const override {
    // Resolve promise with «[ "value" → chunk, "done" → false ]».
    resolver_->Resolve(isolate, ReadResult(isolate, chunk, false));
  }

  void CloseSteps(v8::Isolate* isolate) const override {
    // Resolve promise with «[ "value" → undefined, "done" → true ]».
    resolver_->Resolve(isolate,
                       ReadResult(isolate, v8::Undefined(isolate), true));
  }

  void ErrorSteps(v8::Isolate* isolate,
                  v8::Local<v8::Value> e) const override {
    // Reject promise with e.
    resolver_->Reject(isolate, e);
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(resolver_);
    ReadRequest::Trace(visitor);
  }

 private:
  const cppgc::Member<bindings::ScriptPromiseResolver> resolver_;
};

// static
ReadableStreamDefaultReader* ReadableStreamDefaultReader::Create(
    ReadableStream* stream,
    bindings::ExceptionState& exception_state) {
  auto* const reader =
      bindings::MakeGarbageCollected<ReadableStreamDefaultReader>();
  // Step 1. Perform ? SetUpReadableStreamDefaultReader(this, stream).
  SetUpDefaultReader(exception_state.GetIsolate(), reader, stream,
                     exception_state);
  if (exception_state.HadException()) {
    return nullptr;
  }
  return reader;
}

ReadableStreamDefaultReader::ReadableStreamDefaultReader() = default;

ReadableStreamDefaultReader::~ReadableStreamDefaultReader() = default;

bool ReadableStreamDefaultReader::IsDefaultReader() const {
  return true;
}

bool ReadableStreamDefaultReader::IsBYOBReader() const {
  return false;
}

const bindings::WrapperTypeInfo*
ReadableStreamDefaultReader::GetWrapperTypeInfo() const {
  return &bindings::V8ReadableStreamDefaultReader::kWrapperTypeInfo;
}

void ReadableStreamDefaultReader::Trace(cppgc::Visitor* visitor) const {
  read_requests_.Trace(visitor);
  ReadableStreamGenericReader::Trace(visitor);
}

v8::Local<v8::Promise> ReadableStreamDefaultReader::Read(
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  // Step 1. If this.[[stream]] is undefined, return a promise rejected with a
  // TypeError exception.
  if (!owner_readable_stream_) {
    exception_state.ThrowTypeError(
        "This readable stream reader has been released and cannot be used to "
        "read from its previous owner stream");
    return {};
  }
  // Step 2. Let promise be a new promise.
  auto* const resolver =
      bindings::MakeGarbageCollected<bindings::ScriptPromiseResolver>(isolate);
  // Step 3. Let readRequest be a new read request that settles promise.
  auto* const read_request =
      bindings::MakeGarbageCollected<DefaultReaderReadRequest>(resolver);
  // Step 4. Perform ! ReadableStreamDefaultReaderRead(this, readRequest).
  Read(isolate, this, read_request, exception_state);
  // Step 5. Return promise.
  return resolver->Promise(isolate);
}

// static
void ReadableStreamDefaultReader::Read(
    v8::Isolate* isolate,
    ReadableStreamDefaultReader* reader,
    ReadRequest* read_request,
    bindings::ExceptionState& exception_state) {
  // Steps 1-2. Let stream be reader.[[stream]], which is not undefined.
  ReadableStream* const stream = reader->owner_readable_stream_;
  CHECK(stream);
  // Step 3. Set stream.[[disturbed]] to true.
  stream->is_disturbed_ = true;
  switch (stream->state_) {
    case ReadableStream::kClosed:
      // Step 4. If stream.[[state]] is "closed", perform readRequest's close
      // steps.
      read_request->CloseSteps(isolate);
      break;
    case ReadableStream::kErrored:
      // Step 5. Otherwise, if stream.[[state]] is "errored", perform
      // readRequest's error steps given stream.[[storedError]].
      read_request->ErrorSteps(isolate, stream->GetStoredError(isolate));
      break;
    case ReadableStream::kReadable:
      // Step 6. Otherwise, perform !
      // stream.[[controller]].[[PullSteps]](readRequest).
      stream->GetController()->PullSteps(isolate, read_request,
                                         exception_state);
      break;
  }
}

// static
void ReadableStreamDefaultReader::ErrorReadRequests(
    v8::Isolate* isolate,
    ReadableStreamDefaultReader* reader,
    v8::Local<v8::Value> e) {
  // Steps 1-2. Let readRequests be reader.[[readRequests]], and set
  // reader.[[readRequests]] to a new empty list.
  HeapDeque<ReadRequest> read_requests;
  read_requests.swap(reader->read_requests_);
  // Step 3. For each readRequest of readRequests, perform its error steps,
  // given e.
  for (size_t i = 0; i < read_requests.size(); ++i) {
    read_requests[i]->ErrorSteps(isolate, e);
  }
}

// static
void ReadableStreamDefaultReader::Release(v8::Isolate* isolate,
                                          ReadableStreamDefaultReader* reader) {
  // Step 1. Perform ! ReadableStreamReaderGenericRelease(reader).
  ReadableStreamGenericReader::GenericRelease(isolate, reader);
  // Step 2. Let e be a new TypeError exception.
  const v8::Local<v8::Value> e =
      CreateTypeError(isolate, "Releasing Default reader");
  // Step 3. Perform ! ReadableStreamDefaultReaderErrorReadRequests(reader, e).
  ErrorReadRequests(isolate, reader, e);
}

void ReadableStreamDefaultReader::ReleaseLock(
    bindings::ExceptionState& exception_state) {
  // Step 1. If this.[[stream]] is undefined, return.
  if (!owner_readable_stream_) {
    return;
  }
  // Step 2. Perform ! ReadableStreamDefaultReaderRelease(this).
  Release(exception_state.GetIsolate(), this);
}

// static
void ReadableStreamDefaultReader::SetUpDefaultReader(
    v8::Isolate* isolate,
    ReadableStreamDefaultReader* reader,
    ReadableStream* stream,
    bindings::ExceptionState& exception_state) {
  // Step 1. If ! IsReadableStreamLocked(stream) is true, throw a TypeError
  // exception.
  if (ReadableStream::IsLocked(stream)) {
    exception_state.ThrowTypeError(
        "ReadableStreamDefaultReader constructor can only accept readable "
        "streams that are not yet locked to a reader");
    return;
  }
  // Step 2. Perform ! ReadableStreamReaderGenericInitialize(reader, stream).
  ReadableStreamGenericReader::GenericInitialize(isolate, reader, stream);
  // Step 3. Set reader.[[readRequests]] to a new empty list.
  CHECK(reader->read_requests_.empty());
}

}  // namespace mitto
