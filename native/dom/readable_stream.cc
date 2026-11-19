#include "mitto/dom/readable_stream.h"

#include <stdint.h>

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "base/containers/span.h"
#include "mitto/bindings/async_iterable.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/exception_context.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_function.h"
#include "mitto/bindings/script_promise.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_readable_stream.h"
#include "mitto/dom/byte_stream_tee_engine.h"
#include "mitto/dom/miscellaneous_operations.h"
#include "mitto/dom/pipe_options.h"
#include "mitto/dom/pipe_to_engine.h"
#include "mitto/dom/read_request.h"
#include "mitto/dom/readable_byte_stream_controller.h"
#include "mitto/dom/readable_stream_byob_reader.h"
#include "mitto/dom/readable_stream_controller.h"
#include "mitto/dom/readable_stream_default_controller.h"
#include "mitto/dom/readable_stream_default_reader.h"
#include "mitto/dom/readable_stream_generic_reader.h"
#include "mitto/dom/stream_algorithms.h"
#include "mitto/dom/tee_engine.h"
#include "mitto/dom/transferable_streams.h"
#include "mitto/dom/underlying_byte_source_base.h"
#include "mitto/dom/writable_stream.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

namespace {

// A pull algorithm of a byte stream of the Runtime's own source.
class ByteSourcePullAlgorithm final : public StreamAlgorithm {
 public:
  explicit ByteSourcePullAlgorithm(UnderlyingByteSourceBase* source)
      : source_(source) {}

  v8::Local<v8::Promise> Run(v8::Isolate* isolate,
                             base::span<v8::Local<v8::Value>>) override {
    CHECK(controller_);
    const v8::TryCatch try_catch(isolate);
    bindings::ExceptionState pass_through(isolate);
    const v8::Local<v8::Promise> promise =
        source_->Pull(controller_, pass_through);
    if (try_catch.HasCaught()) {
      return bindings::RejectedPromise(isolate, try_catch.Exception());
    }
    return promise;
  }

  // Must come before Run().
  void SetController(ReadableByteStreamController* controller) {
    controller_ = controller;
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(source_);
    visitor->Trace(controller_);
    StreamAlgorithm::Trace(visitor);
  }

 private:
  const cppgc::Member<UnderlyingByteSourceBase> source_;
  cppgc::Member<ReadableByteStreamController> controller_;
};

// A cancel algorithm of a byte stream of the Runtime's own source.
class ByteSourceCancelAlgorithm final : public StreamAlgorithm {
 public:
  explicit ByteSourceCancelAlgorithm(UnderlyingByteSourceBase* source)
      : source_(source) {}

  v8::Local<v8::Promise> Run(
      v8::Isolate* isolate,
      base::span<v8::Local<v8::Value>> arguments) override {
    CHECK_EQ(arguments.size(), 1u);
    const v8::TryCatch try_catch(isolate);
    const v8::Local<v8::Promise> promise = source_->Cancel(arguments[0]);
    if (try_catch.HasCaught()) {
      return bindings::RejectedPromise(isolate, try_catch.Exception());
    }
    return promise;
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(source_);
    StreamAlgorithm::Trace(visitor);
  }

 private:
  const cppgc::Member<UnderlyingByteSourceBase> source_;
};

// What the constructor reads of an underlying source of type "bytes" as the
// UnderlyingSource dictionary, as Blink's bindings convert it: its members in
// the order of their names, with Chrome's errors.
struct UnderlyingSourceDictionary {
  std::optional<uint64_t> auto_allocate_chunk_size;
  v8::Local<v8::Value> cancel;
  v8::Local<v8::Value> pull;
  v8::Local<v8::Value> start;
};

bool ReadUnderlyingSourceDictionary(v8::Isolate* isolate,
                                    v8::Local<v8::Object> object,
                                    bindings::ExceptionState& exception_state,
                                    UnderlyingSourceDictionary* dictionary) {
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  bindings::DictionaryConversionScope scope(isolate, "UnderlyingSource");
  {
    scope.SetCurrentPropertyName("autoAllocateChunkSize");
    v8::Local<v8::Value> value;
    if (!object
             ->Get(context, v8::String::NewFromUtf8Literal(
                                isolate, "autoAllocateChunkSize"))
             .ToLocal(&value)) {
      return false;
    }
    if (!value->IsUndefined()) {
      uint64_t converted = 0;
      if (!bindings::FromV8IntegerEnforceRange(isolate, value,
                                               "unsigned long long",
                                               exception_state, &converted)) {
        return false;
      }
      dictionary->auto_allocate_chunk_size = converted;
    }
  }
  static constexpr std::array<const char*, 3> kCallbacks = {"cancel", "pull",
                                                           "start"};
  std::array<v8::Local<v8::Value>, kCallbacks.size()> callbacks;
  for (size_t i = 0; i < kCallbacks.size(); ++i) {
    scope.SetCurrentPropertyName(kCallbacks[i]);
    v8::Local<v8::Value> value;
    if (!object
             ->Get(context, v8::String::NewFromUtf8(isolate, kCallbacks[i])
                                .ToLocalChecked())
             .ToLocal(&value)) {
      return false;
    }
    if (!value->IsUndefined() && !value->IsFunction()) {
      exception_state.ThrowTypeError("The given value is not a function.");
      return false;
    }
    callbacks[i] = value;
  }
  dictionary->cancel = callbacks[0];
  dictionary->pull = callbacks[1];
  dictionary->start = callbacks[2];
  {
    scope.SetCurrentPropertyName("type");
    v8::Local<v8::Value> value;
    if (!object->Get(context, v8::String::NewFromUtf8Literal(isolate, "type"))
             .ToLocal(&value)) {
      return false;
    }
    if (!value->IsUndefined()) {
      static constexpr std::u16string_view kTypes[] = {u"bytes"};
      std::u16string converted;
      if (!bindings::FromV8Enumeration(isolate, value, kTypes,
                                       "ReadableStreamType", &converted)) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

class ReadableStream::IterationSource final
    : public bindings::AsyncIterationSourceBase {
 public:
  IterationSource(ReadableStreamDefaultReader* reader, bool prevent_cancel)
      : reader_(reader), prevent_cancel_(prevent_cancel) {}

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(reader_);
    bindings::AsyncIterationSourceBase::Trace(visitor);
  }

 protected:
  void GetNextIterationResult() override;
  void AsyncIteratorReturn(v8::Local<v8::Value> arg) override;

 private:
  friend class IterationReadRequest;

  const cppgc::Member<ReadableStreamDefaultReader> reader_;
  const bool prevent_cancel_;
};

class ReadableStream::IterationReadRequest final : public ReadRequest {
 public:
  explicit IterationReadRequest(IterationSource* iteration_source)
      : iteration_source_(iteration_source) {}

  void ChunkSteps(v8::Isolate* isolate,
                  v8::Local<v8::Value> chunk,
                  bindings::ExceptionState&) const override {
    // Resolve promise with chunk.
    iteration_source_->TakePendingPromiseResolver()->Resolve(
        isolate, iteration_source_->MakeIterationResult(isolate, chunk));
  }

  void CloseSteps(v8::Isolate* isolate) const override {
    // Perform ! ReadableStreamDefaultReaderRelease(reader), and resolve
    // promise with end of iteration.
    ReadableStreamDefaultReader::Release(isolate, iteration_source_->reader_);
    iteration_source_->TakePendingPromiseResolver()->Resolve(
        isolate, iteration_source_->MakeEndOfIteration(isolate));
  }

  void ErrorSteps(v8::Isolate* isolate,
                  v8::Local<v8::Value> e) const override {
    // Perform ! ReadableStreamDefaultReaderRelease(reader), and reject
    // promise with e.
    ReadableStreamDefaultReader::Release(isolate, iteration_source_->reader_);
    iteration_source_->TakePendingPromiseResolver()->Reject(isolate, e);
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(iteration_source_);
    ReadRequest::Trace(visitor);
  }

 private:
  const cppgc::Member<IterationSource> iteration_source_;
};

void ReadableStream::IterationSource::GetNextIterationResult() {
  CHECK(HasPendingPromise());
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  // Step 2. Assert: reader.[[stream]] is not undefined.
  CHECK(reader_->owner_readable_stream_);
  // Steps 4-5. Read from the reader with a new read request.
  auto* const read_request =
      bindings::MakeGarbageCollected<IterationReadRequest>(this);
  bindings::ExceptionState pass_through(isolate);
  ReadableStreamDefaultReader::Read(isolate, reader_, read_request,
                                    pass_through);
}

void ReadableStream::IterationSource::AsyncIteratorReturn(
    v8::Local<v8::Value> arg) {
  CHECK(HasPendingPromise());
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  // Steps 2-3. Assert: reader.[[stream]] is not undefined, and
  // reader.[[readRequests]] is empty.
  CHECK(reader_->owner_readable_stream_);
  CHECK(reader_->read_requests_.empty());
  // Step 4. If iterator's prevent cancel is false: let result be !
  // ReadableStreamReaderGenericCancel(reader, arg), perform !
  // ReadableStreamDefaultReaderRelease(reader), and return result.
  if (!prevent_cancel_) {
    const v8::Local<v8::Promise> result =
        ReadableStreamGenericReader::GenericCancel(isolate, reader_, arg);
    ReadableStreamDefaultReader::Release(isolate, reader_);
    TakePendingPromiseResolver()->Resolve(isolate, result);
    return;
  }
  // Steps 5-6. Perform ! ReadableStreamDefaultReaderRelease(reader), and
  // return a promise resolved with undefined.
  ReadableStreamDefaultReader::Release(isolate, reader_);
  TakePendingPromiseResolver()->Resolve(isolate, v8::Undefined(isolate));
}

// static
ReadableStream* ReadableStream::Create(
    const std::optional<v8::Local<v8::Value>>& underlying_source,
    const std::optional<v8::Local<v8::Value>>& strategy,
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  auto* const stream = bindings::MakeGarbageCollected<ReadableStream>();
  stream->InitInternal(isolate,
                       underlying_source.value_or(v8::Undefined(isolate)),
                       strategy.value_or(v8::Undefined(isolate)),
                       /*created_by_ua=*/false, exception_state);
  if (exception_state.HadException()) {
    return nullptr;
  }
  return stream;
}

// static
ReadableStream* ReadableStream::Create(
    v8::Isolate* isolate,
    StreamStartAlgorithm* start_algorithm,
    StreamAlgorithm* pull_algorithm,
    StreamAlgorithm* cancel_algorithm,
    double high_water_mark,
    StrategySizeAlgorithm* size_algorithm,
    bindings::ExceptionState& exception_state) {
  // Steps 1-2 are the caller's: every argument is required here. Step 3.
  // Assert: ! IsNonNegativeNumber(highWaterMark) is true.
  CHECK_GE(high_water_mark, 0);
  // Steps 4-5. Let stream be a new ReadableStream, and initialize it.
  auto* const stream = bindings::MakeGarbageCollected<ReadableStream>();
  Initialize(stream);
  // Step 6. Let controller be a new ReadableStreamDefaultController.
  auto* const controller =
      bindings::MakeGarbageCollected<ReadableStreamDefaultController>();
  // Step 7. Perform ? SetUpReadableStreamDefaultController(stream,
  // controller, startAlgorithm, pullAlgorithm, cancelAlgorithm,
  // highWaterMark, sizeAlgorithm).
  ReadableStreamDefaultController::SetUp(
      isolate, stream, controller, start_algorithm, pull_algorithm,
      cancel_algorithm, high_water_mark, size_algorithm, exception_state);
  if (exception_state.HadException()) {
    return nullptr;
  }
  // Step 8. Return stream.
  return stream;
}

// static
ReadableStream* ReadableStream::CreateByteStream(
    v8::Isolate* isolate,
    StreamStartAlgorithm* start_algorithm,
    StreamAlgorithm* pull_algorithm,
    StreamAlgorithm* cancel_algorithm,
    bindings::ExceptionState& exception_state) {
  // Steps 1-2. Let stream be a new ReadableStream, and initialize it.
  auto* const stream = bindings::MakeGarbageCollected<ReadableStream>();
  Initialize(stream);
  // Step 3. Let controller be a new ReadableByteStreamController.
  auto* const controller =
      bindings::MakeGarbageCollected<ReadableByteStreamController>();
  // Step 4. Perform ? SetUpReadableByteStreamController(stream, controller,
  // startAlgorithm, pullAlgorithm, cancelAlgorithm, 0, undefined).
  ReadableByteStreamController::SetUp(isolate, stream, controller,
                                      start_algorithm, pull_algorithm,
                                      cancel_algorithm, 0, 0, exception_state);
  if (exception_state.HadException()) {
    return nullptr;
  }
  // Step 5. Return stream.
  return stream;
}

// static
ReadableStream* ReadableStream::CreateByteStream(
    v8::Isolate* isolate,
    UnderlyingByteSourceBase* underlying_byte_source) {
  auto* const stream = bindings::MakeGarbageCollected<ReadableStream>();
  // It cannot fail: the trivial start algorithm does not throw.
  bindings::ExceptionState exception_state(isolate);
  InitByteStream(isolate, stream, underlying_byte_source, exception_state);
  CHECK(!exception_state.HadException());
  return stream;
}

// static
void ReadableStream::InitByteStream(
    v8::Isolate* isolate,
    ReadableStream* stream,
    UnderlyingByteSourceBase* underlying_byte_source,
    bindings::ExceptionState& exception_state) {
  CHECK(stream);
  auto* const pull_algorithm =
      bindings::MakeGarbageCollected<ByteSourcePullAlgorithm>(
          underlying_byte_source);
  auto* const cancel_algorithm =
      bindings::MakeGarbageCollected<ByteSourceCancelAlgorithm>(
          underlying_byte_source);
  // Step 3 of CreateReadableByteStream(). Let controller be a new
  // ReadableByteStreamController.
  auto* const controller =
      bindings::MakeGarbageCollected<ReadableByteStreamController>();
  InitByteStream(isolate, stream, controller, CreateTrivialStartAlgorithm(),
                 pull_algorithm, cancel_algorithm, exception_state);
  CHECK(!exception_state.HadException());
  pull_algorithm->SetController(controller);
}

// static
void ReadableStream::InitByteStream(
    v8::Isolate* isolate,
    ReadableStream* stream,
    ReadableByteStreamController* controller,
    StreamStartAlgorithm* start_algorithm,
    StreamAlgorithm* pull_algorithm,
    StreamAlgorithm* cancel_algorithm,
    bindings::ExceptionState& exception_state) {
  CHECK(stream);
  // Steps 2 and 4 of CreateReadableByteStream().
  Initialize(stream);
  ReadableByteStreamController::SetUp(isolate, stream, controller,
                                      start_algorithm, pull_algorithm,
                                      cancel_algorithm, 0, 0, exception_state);
}

// static
bindings::ScriptWrappable* ReadableStream::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  auto* const stream =
      static_cast<ReadableStream*>(bindings::MakeForKnownGap<ReadableStream>(
          info, bindings::V8ReadableStream::kWrapperTypeInfo));
  Initialize(stream);
  auto* const controller =
      bindings::MakeGarbageCollected<ReadableStreamDefaultController>();
  bindings::ExceptionState exception_state(isolate);
  ReadableStreamDefaultController::SetUp(
      isolate, stream, controller, CreateTrivialStartAlgorithm(),
      CreateTrivialStreamAlgorithm(), CreateTrivialStreamAlgorithm(),
      /*high_water_mark=*/1, CreateDefaultSizeAlgorithm(), exception_state);
  CHECK(!exception_state.HadException());
  ReadableStreamDefaultController::Close(isolate, controller);
  return stream;
}

ReadableStream::ReadableStream() = default;

ReadableStream::~ReadableStream() = default;

const bindings::WrapperTypeInfo* ReadableStream::GetWrapperTypeInfo() const {
  return &bindings::V8ReadableStream::kWrapperTypeInfo;
}

v8::Local<v8::Promise> ReadableStream::Cancel(
    const std::optional<v8::Local<v8::Value>>& reason,
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  // Step 1. If ! IsReadableStreamLocked(this) is true, return a promise
  // rejected with a TypeError exception.
  if (IsLocked(this)) {
    exception_state.ThrowTypeError("Cannot cancel a locked stream");
    return {};
  }
  // Step 2. Return ! ReadableStreamCancel(this, reason).
  return Cancel(isolate, this, reason.value_or(v8::Undefined(isolate)));
}

bindings::ScriptWrappable* ReadableStream::GetReader(
    const std::optional<bindings::ReadableStreamGetReaderOptions>& options,
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  // Step 1. If options["mode"] does not exist, return ?
  // AcquireReadableStreamDefaultReader(this).
  if (!options || !options->mode) {
    return AcquireDefaultReader(isolate, this, exception_state);
  }
  // Step 2. Assert: options["mode"] is "byob". Step 3. Return ?
  // AcquireReadableStreamBYOBReader(this).
  CHECK(*options->mode == u"byob");
  return AcquireBYOBReader(isolate, this, exception_state);
}

ReadableStream* ReadableStream::PipeThrough(
    const bindings::ReadableWritablePair& transform,
    const std::optional<bindings::StreamPipeOptions>& options,
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  CHECK(transform.readable.has_value());
  ReadableStream* const readable_stream = *transform.readable;
  CHECK(transform.writable.has_value());
  WritableStream* const writable_stream = *transform.writable;
  // Step 1. If ! IsReadableStreamLocked(this) is true, throw a TypeError
  // exception.
  if (IsLocked(this)) {
    exception_state.ThrowTypeError("Cannot pipe a locked stream");
    return nullptr;
  }
  // Step 2. If ! IsWritableStreamLocked(transform["writable"]) is true, throw
  // a TypeError exception.
  if (WritableStream::IsLocked(writable_stream)) {
    exception_state.ThrowTypeError("parameter 1's 'writable' is locked");
    return nullptr;
  }
  // Step 3. Let signal be options["signal"] if it exists, or undefined
  // otherwise.
  auto* const pipe_options =
      bindings::MakeGarbageCollected<PipeOptions>(options);
  // Step 4. Let promise be ! ReadableStreamPipeTo(this,
  // transform["writable"], the options, signal).
  const v8::Local<v8::Promise> promise =
      PipeTo(isolate, this, writable_stream, pipe_options, exception_state);
  // Step 5. Set promise.[[PromiseIsHandled]] to true.
  if (!promise.IsEmpty()) {
    promise->MarkAsHandled();
  }
  // Step 6. Return transform["readable"].
  return readable_stream;
}

v8::Local<v8::Promise> ReadableStream::PipeTo(
    WritableStream* destination,
    const std::optional<bindings::StreamPipeOptions>& options,
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  // Step 1. If ! IsReadableStreamLocked(this) is true, return a promise
  // rejected with a TypeError exception.
  if (IsLocked(this)) {
    exception_state.ThrowTypeError("Cannot pipe a locked stream");
    return {};
  }
  // Step 2. If ! IsWritableStreamLocked(destination) is true, return a
  // promise rejected with a TypeError exception.
  if (WritableStream::IsLocked(destination)) {
    exception_state.ThrowTypeError("Cannot pipe to a locked stream");
    return {};
  }
  // Step 3. Let signal be options["signal"] if it exists, or undefined
  // otherwise.
  auto* const pipe_options =
      bindings::MakeGarbageCollected<PipeOptions>(options);
  // Step 4. Return ! ReadableStreamPipeTo(this, destination, the options,
  // signal).
  return PipeTo(isolate, this, destination, pipe_options, exception_state);
}

std::vector<ReadableStream*> ReadableStream::Tee(
    bindings::ExceptionState& exception_state) {
  return CallTeeAndReturnBranchArray(exception_state.GetIsolate(), this,
                                     /*clone_for_branch2=*/false,
                                     exception_state);
}

bindings::AsyncIterationSourceBase* ReadableStream::CreateAsyncIterationSource(
    const bindings::ReadableStreamIteratorOptions& options,
    bindings::ExceptionState& exception_state) {
  // Step 1. Let reader be ? AcquireReadableStreamDefaultReader(stream).
  ReadableStreamDefaultReader* const reader =
      AcquireDefaultReader(exception_state.GetIsolate(), this, exception_state);
  if (!reader) {
    return nullptr;
  }
  // Step 3. Let preventCancel be args[0]["preventCancel"].
  return bindings::MakeGarbageCollected<IterationSource>(
      reader, options.prevent_cancel);
}

void ReadableStream::InitInternal(v8::Isolate* isolate,
                                  v8::Local<v8::Value> raw_underlying_source,
                                  v8::Local<v8::Value> raw_strategy,
                                  bool created_by_ua,
                                  bindings::ExceptionState& exception_state) {
  // Step 1. Perform ! InitializeReadableStream(this).
  Initialize(this);
  // The object conversions that the standard's definition implies.
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Object> underlying_source;
  ScriptValueToObject(isolate, raw_underlying_source, &underlying_source,
                      exception_state);
  if (exception_state.HadException()) {
    return;
  }
  // Steps 2-3. Let size be ? GetV(strategy, "size"), and highWaterMark ?
  // GetV(strategy, "highWaterMark").
  const StrategyUnpacker strategy_unpacker(isolate, raw_strategy,
                                           exception_state);
  if (exception_state.HadException()) {
    return;
  }
  // Step 4. Let type be ? GetV(underlyingSource, "type").
  v8::Local<v8::Value> type;
  {
    const bindings::TryRethrowScope rethrow_scope(isolate, exception_state);
    if (!underlying_source
             ->Get(context, v8::String::NewFromUtf8Literal(isolate, "type"))
             .ToLocal(&type)) {
      return;
    }
  }
  if (!type->IsUndefined()) {
    // Step 5. Let typeString be ? ToString(type).
    v8::Local<v8::String> type_string;
    {
      const bindings::TryRethrowScope rethrow_scope(isolate, exception_state);
      if (!type->ToString(context).ToLocal(&type_string)) {
        return;
      }
    }
    // Step 6. If typeString is "bytes":
    if (type_string->StringEquals(
            v8::String::NewFromUtf8Literal(isolate, "bytes"))) {
      UnderlyingSourceDictionary underlying_source_dict;
      if (!ReadUnderlyingSourceDictionary(isolate, underlying_source,
                                          exception_state,
                                          &underlying_source_dict)) {
        return;
      }
      if (!strategy_unpacker.IsSizeUndefined()) {
        exception_state.ThrowRangeError(
            "Cannot create byte stream with size() defined on the strategy");
        return;
      }
      const double high_water_mark =
          strategy_unpacker.GetHighWaterMark(isolate, 0, exception_state);
      if (exception_state.HadException()) {
        return;
      }
      ReadableByteStreamController::SetUpFromUnderlyingSource(
          isolate, this, underlying_source, underlying_source_dict.start,
          underlying_source_dict.pull, underlying_source_dict.cancel,
          underlying_source_dict.auto_allocate_chunk_size, high_water_mark,
          exception_state);
      return;
    }
    // Step 8. Otherwise, throw a RangeError exception.
    exception_state.ThrowRangeError("Invalid type is specified");
    return;
  }
  // Step 7. Otherwise, if type is undefined: let sizeAlgorithm be ?
  // MakeSizeAlgorithmFromSizeFunction(size).
  StrategySizeAlgorithm* const size_algorithm =
      strategy_unpacker.MakeSizeAlgorithm(isolate, exception_state);
  if (exception_state.HadException()) {
    return;
  }
  CHECK(size_algorithm);
  // If highWaterMark is undefined, let highWaterMark be 1, and set it to ?
  // ValidateAndNormalizeHighWaterMark(highWaterMark).
  const double high_water_mark =
      strategy_unpacker.GetHighWaterMark(isolate, 1, exception_state);
  if (exception_state.HadException()) {
    return;
  }
  // Perform ? SetUpReadableStreamDefaultControllerFromUnderlyingSource(this,
  // underlyingSource, highWaterMark, sizeAlgorithm).
  ReadableStreamDefaultController::SetUpFromUnderlyingSource(
      isolate, this, underlying_source, high_water_mark, size_algorithm,
      exception_state);
}

// static
ReadableStreamDefaultReader* ReadableStream::AcquireDefaultReader(
    v8::Isolate* isolate,
    ReadableStream* stream,
    bindings::ExceptionState& exception_state) {
  CHECK(stream);
  // Steps 1-3. Let reader be a new ReadableStreamDefaultReader, set it up
  // with stream, and return it.
  auto* const reader =
      bindings::MakeGarbageCollected<ReadableStreamDefaultReader>();
  ReadableStreamDefaultReader::SetUpDefaultReader(isolate, reader, stream,
                                                  exception_state);
  if (exception_state.HadException()) {
    return nullptr;
  }
  return reader;
}

// static
ReadableStreamBYOBReader* ReadableStream::AcquireBYOBReader(
    v8::Isolate* isolate,
    ReadableStream* stream,
    bindings::ExceptionState& exception_state) {
  CHECK(stream);
  // Steps 1-3. Let reader be a new ReadableStreamBYOBReader, set it up with
  // stream, and return it.
  auto* const reader =
      bindings::MakeGarbageCollected<ReadableStreamBYOBReader>();
  ReadableStreamBYOBReader::SetUpBYOBReader(isolate, reader, stream,
                                            exception_state);
  if (exception_state.HadException()) {
    return nullptr;
  }
  return reader;
}

// static
void ReadableStream::Initialize(ReadableStream* stream) {
  // The constructor initialized the fields: step 1, the state is "readable";
  // step 2, no reader and no stored error; step 3, not disturbed.
  CHECK_EQ(stream->state_, kReadable);
  CHECK(!stream->reader_);
  CHECK(stream->stored_error_.IsEmpty());
  CHECK(!stream->is_disturbed_);
}

void ReadableStream::Tee(v8::Isolate* isolate,
                         ReadableStream** branch1,
                         ReadableStream** branch2,
                         bool clone_for_branch2,
                         bindings::ExceptionState& exception_state) {
  auto* const engine = bindings::MakeGarbageCollected<TeeEngine>();
  engine->Start(isolate, this, clone_for_branch2, exception_state);
  if (exception_state.HadException()) {
    return;
  }
  // The branches, which the standard returns as a list.
  *branch1 = engine->Branch1();
  *branch2 = engine->Branch2();
}

void ReadableStream::ByteStreamTee(v8::Isolate* isolate,
                                   ReadableStream** branch1,
                                   ReadableStream** branch2,
                                   bindings::ExceptionState& exception_state) {
  auto* const engine = bindings::MakeGarbageCollected<ByteStreamTeeEngine>();
  engine->Start(isolate, this, exception_state);
  if (exception_state.HadException()) {
    return;
  }
  // The branches, which the standard returns as a list.
  *branch1 = engine->Branch1();
  *branch2 = engine->Branch2();
}

void ReadableStream::LockAndDisturb(v8::Isolate* isolate) {
  if (reader_) {
    return;
  }
  // The stream is not locked, so this cannot fail.
  bindings::ExceptionState exception_state(isolate);
  ReadableStreamGenericReader* const reader =
      AcquireDefaultReader(isolate, this, exception_state);
  CHECK(reader);
  is_disturbed_ = true;
}

void ReadableStream::CloseStream(v8::Isolate* isolate,
                                 bindings::ExceptionState& exception_state) {
  // Step 1. If stream.[[controller]] implements ReadableByteStreamController:
  if (readable_stream_controller_->IsByteStreamController()) {
    auto* const controller = static_cast<ReadableByteStreamController*>(
        readable_stream_controller_.Get());
    // Step 1.1. Perform ! ReadableByteStreamControllerClose(
    // stream.[[controller]]).
    {
      const bindings::TryRethrowScope rethrow_scope(isolate, exception_state);
      ReadableByteStreamController::Close(isolate, controller);
      if (rethrow_scope.HasCaught()) {
        return;
      }
    }
    // Step 1.2. If stream.[[controller]].[[pendingPullIntos]] is not empty,
    // perform ! ReadableByteStreamControllerRespond(stream.[[controller]], 0).
    if (controller->pending_pull_intos_.size() > 0) {
      ReadableByteStreamController::Respond(isolate, controller, 0,
                                            exception_state);
    }
    return;
  }
  // Step 2. Otherwise, perform ! ReadableStreamDefaultControllerClose(
  // stream.[[controller]]).
  ReadableStreamDefaultController::Close(
      isolate, static_cast<ReadableStreamDefaultController*>(
                   readable_stream_controller_.Get()));
}

// static
v8::Local<v8::Promise> ReadableStream::PipeTo(
    v8::Isolate* isolate,
    ReadableStream* readable,
    WritableStream* destination,
    PipeOptions* pipe_options,
    bindings::ExceptionState& exception_state) {
  CHECK(destination);
  auto* const engine =
      bindings::MakeGarbageCollected<PipeToEngine>(isolate, pipe_options);
  return engine->Start(readable, destination, exception_state);
}

void ReadableStream::Serialize(v8::Isolate* isolate,
                               MessagePort* port,
                               bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#rs-transfer
  // 1. If ! IsReadableStreamLocked(value) is true, throw a "DataCloneError"
  //    DOMException.
  if (IsLocked(this)) {
    exception_state.ThrowTypeError("Cannot transfer a locked stream");
    return;
  }
  // Done by SerializedScriptValue's transfer of the stream:
  // 2. Let port1 be a new MessagePort in the current Realm.
  // 3. Let port2 be a new MessagePort in the current Realm.
  // 4. Entangle port1 and port2.
  // 5. Let writable be a new WritableStream in the current Realm.
  // 6. Perform ! SetUpCrossRealmTransformWritable(writable, port1).
  WritableStream* const writable =
      CreateCrossRealmTransformWritable(isolate, port, exception_state);
  if (exception_state.HadException()) {
    return;
  }
  // 7. Let promise be ! ReadableStreamPipeTo(value, writable, false, false,
  //    false).
  const v8::Local<v8::Promise> promise =
      PipeTo(isolate, this, writable,
             bindings::MakeGarbageCollected<PipeOptions>(), exception_state);
  // 8. Set promise.[[PromiseIsHandled]] to true.
  if (!promise.IsEmpty()) {
    promise->MarkAsHandled();
  }
  // This step is done by the caller:
  // 9. Set dataHolder.[[port]] to ! StructuredSerializeWithTransfer(port2,
  //    « port2 »).
}

// static
ReadableStream* ReadableStream::Deserialize(
    v8::Isolate* isolate,
    MessagePort* port,
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#rs-transfer
  // Steps 1 and 2 are done by the deserializer, which gives `port`.
  // We need to execute JavaScript to call "Then" on v8::Promises, inside the
  // deserializer, which forbids it. We will not run author code.
  const v8::Isolate::AllowJavascriptExecutionScope allow_javascript(isolate);
  // 3. Perform ! SetUpCrossRealmTransformReadable(value, port).
  return CreateCrossRealmTransformReadable(isolate, port, exception_state);
}

v8::Local<v8::Value> ReadableStream::GetStoredError(
    v8::Isolate* isolate) const {
  return stored_error_.Get(isolate);
}

void ReadableStream::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(readable_stream_controller_);
  visitor->Trace(reader_);
  visitor->Trace(stored_error_);
  bindings::ScriptWrappable::Trace(visitor);
}

// static
void ReadableStream::AddReadIntoRequest(v8::Isolate* isolate,
                                        ReadableStream* stream,
                                        ReadIntoRequest* read_request) {
  CHECK(stream);
  // Step 1. Assert: stream.[[reader]] implements ReadableStreamBYOBReader.
  CHECK(stream->reader_->IsBYOBReader());
  // Step 2. Assert: stream.[[state]] is "readable" or "closed".
  CHECK(stream->state_ == kReadable || stream->state_ == kClosed);
  // Step 3. Append readRequest to stream.[[reader]].[[readIntoRequests]].
  static_cast<ReadableStreamBYOBReader*>(stream->reader_.Get())
      ->read_into_requests_.push_back(read_request);
}

// static
void ReadableStream::AddReadRequest(v8::Isolate* isolate,
                                    ReadableStream* stream,
                                    ReadRequest* read_request) {
  CHECK(stream);
  // Step 1. Assert: stream.[[reader]] implements ReadableStreamDefaultReader.
  CHECK(stream->reader_->IsDefaultReader());
  // Step 2. Assert: stream.[[state]] is "readable".
  CHECK_EQ(stream->state_, kReadable);
  // Step 3. Append readRequest to stream.[[reader]].[[readRequests]].
  static_cast<ReadableStreamDefaultReader*>(stream->reader_.Get())
      ->read_requests_.push_back(read_request);
}

// static
v8::Local<v8::Promise> ReadableStream::Cancel(v8::Isolate* isolate,
                                              ReadableStream* stream,
                                              v8::Local<v8::Value> reason) {
  CHECK(stream);
  // Step 1. Set stream.[[disturbed]] to true.
  stream->is_disturbed_ = true;
  // Step 2. If stream.[[state]] is "closed", return a promise resolved with
  // undefined.
  if (stream->state_ == kClosed) {
    return bindings::ResolvedUndefinedPromise(isolate);
  }
  // Step 3. If stream.[[state]] is "errored", return a promise rejected with
  // stream.[[storedError]].
  if (stream->state_ == kErrored) {
    return bindings::RejectedPromise(isolate, stream->GetStoredError(isolate));
  }
  // Step 4. Perform ! ReadableStreamClose(stream).
  Close(isolate, stream);
  // Step 5. Let reader be stream.[[reader]].
  ReadableStreamGenericReader* const reader = stream->reader_;
  // Step 6. If reader is not undefined and implements
  // ReadableStreamBYOBReader: empty reader.[[readIntoRequests]], and perform
  // each request's close steps, given undefined.
  if (reader && reader->IsBYOBReader()) {
    HeapDeque<ReadIntoRequest> read_into_requests;
    read_into_requests.swap(
        static_cast<ReadableStreamBYOBReader*>(reader)->read_into_requests_);
    for (size_t i = 0; i < read_into_requests.size(); ++i) {
      read_into_requests[i]->CloseSteps(isolate,
                                        v8::Local<v8::ArrayBufferView>());
    }
  }
  // Step 7. Let sourceCancelPromise be !
  // stream.[[controller]].[[CancelSteps]](reason).
  const v8::Local<v8::Promise> source_cancel_promise =
      stream->readable_stream_controller_->CancelSteps(isolate, reason);

  class ResolveUndefinedFunction final : public bindings::ScriptFunction {
   public:
    v8::Local<v8::Value> Call(v8::Isolate* isolate,
                              v8::Local<v8::Value>) override {
      return v8::Undefined(isolate);
    }
  };

  // Step 8. Return the result of reacting to sourceCancelPromise with a
  // fulfillment step that returns undefined.
  return bindings::PromiseThen(
      isolate, source_cancel_promise,
      bindings::MakeGarbageCollected<ResolveUndefinedFunction>());
}

// static
void ReadableStream::Close(v8::Isolate* isolate, ReadableStream* stream) {
  CHECK(stream);
  // Step 1. Assert: stream.[[state]] is "readable".
  CHECK_EQ(stream->state_, kReadable);
  // Step 2. Set stream.[[state]] to "closed".
  stream->state_ = kClosed;
  // Steps 3-4. Let reader be stream.[[reader]]; if it is undefined, return.
  ReadableStreamGenericReader* const reader = stream->reader_;
  if (!reader) {
    return;
  }
  // Step 5. Resolve reader.[[closedPromise]] with undefined.
  reader->ClosedResolver()->Resolve(isolate);
  // Step 6. If reader implements ReadableStreamDefaultReader: empty
  // reader.[[readRequests]], and perform each request's close steps.
  if (reader->IsDefaultReader()) {
    HeapDeque<ReadRequest> requests;
    requests.swap(
        static_cast<ReadableStreamDefaultReader*>(reader)->read_requests_);
    for (size_t i = 0; i < requests.size(); ++i) {
      requests[i]->CloseSteps(isolate);
    }
  }
}

// static
void ReadableStream::Error(v8::Isolate* isolate,
                           ReadableStream* stream,
                           v8::Local<v8::Value> e) {
  CHECK(stream);
  // Step 1. Assert: stream.[[state]] is "readable".
  CHECK_EQ(stream->state_, kReadable);
  // Step 2. Set stream.[[state]] to "errored".
  stream->state_ = kErrored;
  // Step 3. Set stream.[[storedError]] to e.
  stream->stored_error_.Reset(isolate, e);
  // Steps 4-5. Let reader be stream.[[reader]]; if it is undefined, return.
  ReadableStreamGenericReader* const reader = stream->reader_;
  if (!reader) {
    return;
  }
  // Step 6. Reject reader.[[closedPromise]] with e.
  reader->ClosedResolver()->Reject(isolate, e);
  // Step 7. Set reader.[[closedPromise]].[[PromiseIsHandled]] to true.
  reader->Closed()->MarkAsHandled();
  if (reader->IsDefaultReader()) {
    // Step 8. If reader implements ReadableStreamDefaultReader, perform !
    // ReadableStreamDefaultReaderErrorReadRequests(reader, e).
    ReadableStreamDefaultReader::ErrorReadRequests(
        isolate, static_cast<ReadableStreamDefaultReader*>(reader), e);
  } else {
    // Step 9. Otherwise, reader implements ReadableStreamBYOBReader: perform
    // ! ReadableStreamBYOBReaderErrorReadIntoRequests(reader, e).
    CHECK(reader->IsBYOBReader());
    ReadableStreamBYOBReader::ErrorReadIntoRequests(
        isolate, static_cast<ReadableStreamBYOBReader*>(reader), e);
  }
}

// static
void ReadableStream::FulfillReadIntoRequest(
    v8::Isolate* isolate,
    ReadableStream* stream,
    v8::Local<v8::ArrayBufferView> chunk,
    bool done,
    bindings::ExceptionState& exception_state) {
  CHECK(stream);
  // Step 1. Assert: ! ReadableStreamHasBYOBReader(stream) is true.
  CHECK(HasBYOBReader(stream));
  // Step 2. Let reader be stream.[[reader]].
  auto* const byob_reader =
      static_cast<ReadableStreamBYOBReader*>(stream->reader_.Get());
  // Step 3. Assert: reader.[[readIntoRequests]] is not empty.
  CHECK(!byob_reader->read_into_requests_.empty());
  // Steps 4-5. Take the first readIntoRequest from reader.[[readIntoRequests]].
  ReadIntoRequest* const read_into_request =
      byob_reader->read_into_requests_.TakeFirst();
  if (done) {
    // Step 6. If done is true, perform readIntoRequest's close steps, given
    // chunk.
    read_into_request->CloseSteps(isolate, chunk);
  } else {
    // Step 7. Otherwise, perform readIntoRequest's chunk steps, given chunk.
    read_into_request->ChunkSteps(isolate, chunk, exception_state);
  }
}

// static
void ReadableStream::FulfillReadRequest(
    v8::Isolate* isolate,
    ReadableStream* stream,
    v8::Local<v8::Value> chunk,
    bool done,
    bindings::ExceptionState& exception_state) {
  CHECK(stream);
  // Step 1. Assert: ! ReadableStreamHasDefaultReader(stream) is true.
  CHECK(HasDefaultReader(stream));
  // Step 2. Let reader be stream.[[reader]].
  auto* const default_reader =
      static_cast<ReadableStreamDefaultReader*>(stream->reader_.Get());
  // Step 3. Assert: reader.[[readRequests]] is not empty.
  CHECK(!default_reader->read_requests_.empty());
  // Steps 4-5. Take the first readRequest from reader.[[readRequests]].
  ReadRequest* const read_request = default_reader->read_requests_.TakeFirst();
  if (done) {
    // Step 6. If done is true, perform readRequest's close steps.
    read_request->CloseSteps(isolate);
  } else {
    // Step 7. Otherwise, perform readRequest's chunk steps, given chunk.
    read_request->ChunkSteps(isolate, chunk, exception_state);
  }
}

// static
int ReadableStream::GetNumReadIntoRequests(const ReadableStream* stream) {
  // Step 1. Assert: ! ReadableStreamHasBYOBReader(stream) is true.
  CHECK(HasBYOBReader(stream));
  // Step 2. Return stream.[[reader]].[[readIntoRequests]]'s size.
  return static_cast<int>(
      static_cast<ReadableStreamBYOBReader*>(stream->reader_.Get())
          ->read_into_requests_.size());
}

// static
int ReadableStream::GetNumReadRequests(const ReadableStream* stream) {
  // Step 1. Assert: ! ReadableStreamHasDefaultReader(stream) is true.
  CHECK(HasDefaultReader(stream));
  // Step 2. Return stream.[[reader]].[[readRequests]]'s size.
  return static_cast<int>(
      static_cast<ReadableStreamDefaultReader*>(stream->reader_.Get())
          ->read_requests_.size());
}

// static
bool ReadableStream::HasBYOBReader(const ReadableStream* stream) {
  // Steps 1-4. Whether stream.[[reader]] is a ReadableStreamBYOBReader.
  const ReadableStreamGenericReader* const reader = stream->reader_;
  return reader && reader->IsBYOBReader();
}

// static
bool ReadableStream::HasDefaultReader(const ReadableStream* stream) {
  // Steps 1-4. Whether stream.[[reader]] is a ReadableStreamDefaultReader.
  const ReadableStreamGenericReader* const reader = stream->reader_;
  return reader && reader->IsDefaultReader();
}

// static
std::vector<ReadableStream*> ReadableStream::CallTeeAndReturnBranchArray(
    v8::Isolate* isolate,
    ReadableStream* readable,
    bool clone_for_branch2,
    bindings::ExceptionState& exception_state) {
  ReadableStream* branch1 = nullptr;
  ReadableStream* branch2 = nullptr;
  // Step 1. Let branches be ? ReadableStreamTee(this, false).
  if (readable->readable_stream_controller_->IsByteStreamController()) {
    readable->ByteStreamTee(isolate, &branch1, &branch2, exception_state);
  } else {
    CHECK(readable->readable_stream_controller_->IsDefaultController());
    readable->Tee(isolate, &branch1, &branch2, clone_for_branch2,
                  exception_state);
  }
  if (!branch1 || !branch2) {
    return {};
  }
  // Step 2. Return ! CreateArrayFromList(branches).
  return {branch1, branch2};
}

}  // namespace mitto
