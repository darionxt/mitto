// The Streams Standard's ReadableStream, as Blink's: a stream of a page's
// source or of the Runtime's, with a default controller or a byte stream
// controller, its readers, tee(), pipeTo() and pipeThrough(), and async
// iteration.
//
// Upstream: third_party/blink/renderer/core/streams/readable_stream.h
// Upstream: third_party/blink/renderer/core/streams/readable_stream.cc

#ifndef MITTO_DOM_READABLE_STREAM_H_
#define MITTO_DOM_READABLE_STREAM_H_

#include <stdint.h>

#include <optional>
#include <vector>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-promise.h"
#include "v8/include/v8-traced-handle.h"
#include "v8/include/v8-typed-array.h"

namespace v8 {
class Isolate;
class Value;
}  // namespace v8

namespace mitto {

namespace bindings {
class AsyncIterationSourceBase;
class ExceptionState;
struct ReadableStreamGetReaderOptions;
struct ReadableStreamIteratorOptions;
struct ReadableWritablePair;
struct StreamPipeOptions;
}  // namespace bindings

class PipeOptions;
class ReadableByteStreamController;
class ReadableStreamBYOBReader;
class ReadableStreamController;
class ReadableStreamDefaultReader;
class ReadableStreamGenericReader;
class ReadIntoRequest;
class ReadRequest;
class StrategySizeAlgorithm;
class StreamAlgorithm;
class StreamStartAlgorithm;
class UnderlyingByteSourceBase;
class WritableStream;

class ReadableStream : public bindings::ScriptWrappable {
 public:
  enum State : uint8_t { kReadable, kClosed, kErrored };

  // `new ReadableStream(underlyingSource, strategy)`.
  static ReadableStream* Create(
      const std::optional<v8::Local<v8::Value>>& underlying_source,
      const std::optional<v8::Local<v8::Value>>& strategy,
      bindings::ExceptionState& exception_state);

  // CreateReadableStream().
  static ReadableStream* Create(v8::Isolate* isolate,
                                StreamStartAlgorithm* start_algorithm,
                                StreamAlgorithm* pull_algorithm,
                                StreamAlgorithm* cancel_algorithm,
                                double high_water_mark,
                                StrategySizeAlgorithm* size_algorithm,
                                bindings::ExceptionState& exception_state);

  // CreateReadableByteStream().
  static ReadableStream* CreateByteStream(
      v8::Isolate* isolate,
      StreamStartAlgorithm* start_algorithm,
      StreamAlgorithm* pull_algorithm,
      StreamAlgorithm* cancel_algorithm,
      bindings::ExceptionState& exception_state);

  // A byte stream of the Runtime's own source, such as a response's body.
  static ReadableStream* CreateByteStream(
      v8::Isolate* isolate,
      UnderlyingByteSourceBase* underlying_byte_source);
  static void InitByteStream(v8::Isolate* isolate,
                             ReadableStream* stream,
                             UnderlyingByteSourceBase* underlying_byte_source,
                             bindings::ExceptionState& exception_state);
  static void InitByteStream(v8::Isolate* isolate,
                             ReadableStream* stream,
                             ReadableByteStreamController* controller,
                             StreamStartAlgorithm* start_algorithm,
                             StreamAlgorithm* pull_algorithm,
                             StreamAlgorithm* cancel_algorithm,
                             bindings::ExceptionState& exception_state);

  // What a Known Gap gives for a stream, such as Blob's stream() until a Blob
  // has bytes: a stream of the interface `info`, ReadableStream's or one that
  // inherits from it, with a default controller, closed and empty.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  ReadableStream();
  ~ReadableStream() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  bool Locked() const { return IsLocked(this); }
  v8::Local<v8::Promise> Cancel(
      const std::optional<v8::Local<v8::Value>>& reason,
      bindings::ExceptionState& exception_state);
  // A ReadableStreamDefaultReader or a ReadableStreamBYOBReader.
  bindings::ScriptWrappable* GetReader(
      const std::optional<bindings::ReadableStreamGetReaderOptions>& options,
      bindings::ExceptionState& exception_state);
  ReadableStream* PipeThrough(
      const bindings::ReadableWritablePair& transform,
      const std::optional<bindings::StreamPipeOptions>& options,
      bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> PipeTo(
      WritableStream* destination,
      const std::optional<bindings::StreamPipeOptions>& options,
      bindings::ExceptionState& exception_state);
  std::vector<ReadableStream*> Tee(bindings::ExceptionState& exception_state);
  // The source of values() and @@asyncIterator.
  bindings::AsyncIterationSourceBase* CreateAsyncIterationSource(
      const bindings::ReadableStreamIteratorOptions& options,
      bindings::ExceptionState& exception_state);

  // ReadableStreamDefaultTee(), with cloneForBranch2.
  void Tee(v8::Isolate* isolate,
           ReadableStream** branch1,
           ReadableStream** branch2,
           bool clone_for_branch2,
           bindings::ExceptionState& exception_state);
  // ReadableByteStreamTee().
  void ByteStreamTee(v8::Isolate* isolate,
                     ReadableStream** branch1,
                     ReadableStream** branch2,
                     bindings::ExceptionState& exception_state);

  bool IsLocked() const { return IsLocked(this); }
  bool IsDisturbed() const { return IsDisturbed(this); }
  bool IsReadable() const { return IsReadable(this); }
  bool IsClosed() const { return IsClosed(this); }
  bool IsErrored() const { return IsErrored(this); }

  // Locks it to a new default reader and marks it disturbed, as reading all
  // of a body does.
  void LockAndDisturb(v8::Isolate* isolate);
  // Marks it disturbed without a reader.
  void MarkDisturbed() { is_disturbed_ = true; }

  // ReadableStreamClose() for a stream that is readable, as a C++ source
  // closes it.
  void CloseStream(v8::Isolate* isolate,
                   bindings::ExceptionState& exception_state);

  // The abstract operations of readable streams.
  //
  // IsReadableStreamDisturbed().
  static bool IsDisturbed(const ReadableStream* stream) {
    return stream->is_disturbed_;
  }
  // IsReadableStreamLocked().
  static bool IsLocked(const ReadableStream* stream) {
    return stream->reader_ != nullptr;
  }
  // ReadableStreamPipeTo().
  static v8::Local<v8::Promise> PipeTo(
      v8::Isolate* isolate,
      ReadableStream* readable,
      WritableStream* destination,
      PipeOptions* pipe_options,
      bindings::ExceptionState& exception_state);
  // AcquireReadableStreamDefaultReader().
  static ReadableStreamDefaultReader* AcquireDefaultReader(
      v8::Isolate* isolate,
      ReadableStream* stream,
      bindings::ExceptionState& exception_state);
  // AcquireReadableStreamBYOBReader().
  static ReadableStreamBYOBReader* AcquireBYOBReader(
      v8::Isolate* isolate,
      ReadableStream* stream,
      bindings::ExceptionState& exception_state);
  // ReadableStreamCancel().
  static v8::Local<v8::Promise> Cancel(v8::Isolate* isolate,
                                       ReadableStream* stream,
                                       v8::Local<v8::Value> reason);

  // Not part of the standard: for TransformStream and the Runtime's own
  // sources.
  static bool IsReadable(const ReadableStream* stream) {
    return stream->state_ == kReadable;
  }
  static bool IsClosed(const ReadableStream* stream) {
    return stream->state_ == kClosed;
  }
  static bool IsErrored(const ReadableStream* stream) {
    return stream->state_ == kErrored;
  }
  ReadableStreamController* GetController() {
    return readable_stream_controller_.Get();
  }
  v8::Local<v8::Value> GetStoredError(v8::Isolate* isolate) const;

 private:
  friend class ByteStreamTeeEngine;
  friend class PipeToEngine;
  friend class ReadableByteStreamController;
  friend class ReadableStreamBYOBReader;
  friend class ReadableStreamDefaultController;
  friend class ReadableStreamDefaultReader;
  friend class ReadableStreamGenericReader;
  friend class TeeEngine;

  class IterationSource;
  class IterationReadRequest;

  // Called once after a read of a byte stream gave bytes. A subclass may
  // override it.
  virtual void DidConsumeBytes(size_t) {}

  // The steps of the constructor.
  void InitInternal(v8::Isolate* isolate,
                    v8::Local<v8::Value> raw_underlying_source,
                    v8::Local<v8::Value> raw_strategy,
                    bool created_by_ua,
                    bindings::ExceptionState& exception_state);

  // InitializeReadableStream().
  static void Initialize(ReadableStream* stream);
  // ReadableStreamAddReadIntoRequest().
  static void AddReadIntoRequest(v8::Isolate* isolate,
                                 ReadableStream* stream,
                                 ReadIntoRequest* read_request);
  // ReadableStreamAddReadRequest().
  static void AddReadRequest(v8::Isolate* isolate,
                             ReadableStream* stream,
                             ReadRequest* read_request);
  // ReadableStreamClose().
  static void Close(v8::Isolate* isolate, ReadableStream* stream);
  // ReadableStreamError().
  static void Error(v8::Isolate* isolate,
                    ReadableStream* stream,
                    v8::Local<v8::Value> e);
  // ReadableStreamFulfillReadIntoRequest(). `chunk` is empty for undefined.
  static void FulfillReadIntoRequest(
      v8::Isolate* isolate,
      ReadableStream* stream,
      v8::Local<v8::ArrayBufferView> chunk,
      bool done,
      bindings::ExceptionState& exception_state);
  // ReadableStreamFulfillReadRequest().
  static void FulfillReadRequest(v8::Isolate* isolate,
                                 ReadableStream* stream,
                                 v8::Local<v8::Value> chunk,
                                 bool done,
                                 bindings::ExceptionState& exception_state);
  // ReadableStreamGetNumReadIntoRequests().
  static int GetNumReadIntoRequests(const ReadableStream* stream);
  // ReadableStreamGetNumReadRequests().
  static int GetNumReadRequests(const ReadableStream* stream);
  // ReadableStreamHasBYOBReader().
  static bool HasBYOBReader(const ReadableStream* stream);
  // ReadableStreamHasDefaultReader().
  static bool HasDefaultReader(const ReadableStream* stream);

  // Tee() of `readable`, with its two branches in a list.
  static std::vector<ReadableStream*> CallTeeAndReturnBranchArray(
      v8::Isolate* isolate,
      ReadableStream* readable,
      bool clone_for_branch2,
      bindings::ExceptionState& exception_state);

  bool is_disturbed_ = false;
  State state_ = kReadable;
  cppgc::Member<ReadableStreamController> readable_stream_controller_;
  cppgc::Member<ReadableStreamGenericReader> reader_;
  v8::TracedReference<v8::Value> stored_error_;
};

}  // namespace mitto

#endif  // MITTO_DOM_READABLE_STREAM_H_
