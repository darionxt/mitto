// The Streams Standard's WritableStream, as Blink's.
//
// Upstream: third_party/blink/renderer/core/streams/writable_stream.h
// Upstream: third_party/blink/renderer/core/streams/writable_stream.cc

#ifndef MITTO_DOM_WRITABLE_STREAM_H_
#define MITTO_DOM_WRITABLE_STREAM_H_

#include <stdint.h>

#include <optional>

#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/heap_deque.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-promise.h"
#include "v8/include/v8-traced-handle.h"

namespace v8 {
class Isolate;
class String;
class Value;
}  // namespace v8

namespace mitto {

namespace bindings {
class ExceptionState;
class ScriptPromiseResolver;
}  // namespace bindings

class StrategySizeAlgorithm;
class StreamAlgorithm;
class MessagePort;
class StreamStartAlgorithm;
class WritableStreamDefaultController;
class WritableStreamDefaultWriter;

class WritableStream : public bindings::ScriptWrappable {
 public:
  enum State : uint8_t {
    kWritable,
    kClosed,
    kErroring,
    kErrored,
  };

  // `new WritableStream(underlyingSink, strategy)`.
  static WritableStream* Create(
      const std::optional<v8::Local<v8::Value>>& underlying_sink,
      const std::optional<v8::Local<v8::Value>>& strategy,
      bindings::ExceptionState& exception_state);

  // CreateWritableStream(), with highWaterMark and sizeAlgorithm required.
  static WritableStream* Create(v8::Isolate* isolate,
                                StreamStartAlgorithm* start_algorithm,
                                StreamAlgorithm* write_algorithm,
                                StreamAlgorithm* close_algorithm,
                                StreamAlgorithm* abort_algorithm,
                                double high_water_mark,
                                StrategySizeAlgorithm* size_algorithm,
                                bindings::ExceptionState& exception_state);

  // What a Known Gap gives for a stream, such as SerialPort's writable: a
  // stream of the interface `info`, WritableStream's or one that inherits
  // from it, whose sink takes each chunk and keeps nothing.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  WritableStream();
  ~WritableStream() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  bool Locked() const { return IsLocked(this); }
  v8::Local<v8::Promise> Abort(
      const std::optional<v8::Local<v8::Value>>& reason,
      bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> Close(bindings::ExceptionState& exception_state);
  WritableStreamDefaultWriter* GetWriter(
      bindings::ExceptionState& exception_state);

  // IsWritableStreamLocked().
  static bool IsLocked(const WritableStream* stream) {
    return stream->writer_ != nullptr;
  }

  // The steps of transferring it
  // (https://streams.spec.whatwg.org/#ws-transfer), whose other end `port`
  // holds: a cross-realm readable stream over `port` pipes into it. Throws for
  // a locked stream.
  void Serialize(v8::Isolate* isolate,
                 MessagePort* port,
                 bindings::ExceptionState& exception_state);
  // The stream that a transfer gives where it arrives, over `port`.
  static WritableStream* Deserialize(v8::Isolate* isolate,
                                     MessagePort* port,
                                     bindings::ExceptionState& exception_state);

  // For ReadableStream's pipeTo().
  //
  // AcquireWritableStreamDefaultWriter().
  static WritableStreamDefaultWriter* AcquireDefaultWriter(
      v8::Isolate* isolate,
      WritableStream* stream,
      bindings::ExceptionState& exception_state);

  // For WritableStreamDefaultWriter.
  //
  // WritableStreamAbort().
  static v8::Local<v8::Promise> Abort(v8::Isolate* isolate,
                                      WritableStream* stream,
                                      v8::Local<v8::Value> reason);
  // WritableStreamAddWriteRequest().
  static void AddWriteRequest(WritableStream* stream,
                              bindings::ScriptPromiseResolver* resolver);
  // WritableStreamClose().
  static v8::Local<v8::Promise> Close(v8::Isolate* isolate,
                                      WritableStream* stream);
  // WritableStreamCloseQueuedOrInFlight().
  static bool CloseQueuedOrInFlight(const WritableStream* stream);

  // For WritableStreamDefaultController.
  //
  // WritableStreamDealWithRejection().
  static void DealWithRejection(v8::Isolate* isolate,
                                WritableStream* stream,
                                v8::Local<v8::Value> error);
  // WritableStreamStartErroring().
  static void StartErroring(v8::Isolate* isolate,
                            WritableStream* stream,
                            v8::Local<v8::Value> reason);
  // WritableStreamFinishErroring().
  static void FinishErroring(v8::Isolate* isolate, WritableStream* stream);
  // WritableStreamFinishInFlightWrite().
  static void FinishInFlightWrite(v8::Isolate* isolate, WritableStream* stream);
  // WritableStreamFinishInFlightWriteWithError().
  static void FinishInFlightWriteWithError(v8::Isolate* isolate,
                                           WritableStream* stream,
                                           v8::Local<v8::Value> error);
  // WritableStreamFinishInFlightClose().
  static void FinishInFlightClose(v8::Isolate* isolate, WritableStream* stream);
  // WritableStreamFinishInFlightCloseWithError().
  static void FinishInFlightCloseWithError(v8::Isolate* isolate,
                                           WritableStream* stream,
                                           v8::Local<v8::Value> error);
  // WritableStreamMarkCloseRequestInFlight().
  static void MarkCloseRequestInFlight(WritableStream* stream);
  // WritableStreamMarkFirstWriteRequestInFlight().
  static void MarkFirstWriteRequestInFlight(WritableStream* stream);
  // WritableStreamUpdateBackpressure().
  static void UpdateBackpressure(v8::Isolate* isolate,
                                 WritableStream* stream,
                                 bool backpressure);

  // For the other stream classes.
  State GetState() const { return state_; }
  bool IsErrored() const { return state_ == kErrored; }
  bool IsErroring() const { return state_ == kErroring; }
  bool IsWritable() const { return state_ == kWritable; }
  bool HasBackpressure() const { return has_backpressure_; }
  bool HasInFlightWriteRequest() const {
    return in_flight_write_request_ != nullptr;
  }
  bool IsClosingOrClosed() const {
    return CloseQueuedOrInFlight(this) || state_ == kClosed;
  }
  v8::Local<v8::Value> GetStoredError(v8::Isolate* isolate) const;
  WritableStreamDefaultController* Controller() {
    return writable_stream_controller_.Get();
  }
  const WritableStreamDefaultController* Controller() const {
    return writable_stream_controller_.Get();
  }
  const WritableStreamDefaultWriter* Writer() const { return writer_.Get(); }
  void SetCloseRequest(bindings::ScriptPromiseResolver* close_request);
  void SetController(WritableStreamDefaultController* controller);
  void SetWriter(WritableStreamDefaultWriter* writer);

  // A message such as "Cannot write to a closing writable stream".
  static v8::Local<v8::String> CreateCannotActionOnStateStreamMessage(
      v8::Isolate* isolate,
      const char* action,
      const char* state_name);
  // A TypeError with that message for `state`.
  static v8::Local<v8::Value> CreateCannotActionOnStateStreamException(
      v8::Isolate* isolate,
      const char* action,
      State state);

 protected:
  // The steps of the constructor.
  void InitInternal(v8::Isolate* isolate,
                    v8::Local<v8::Value> raw_underlying_sink,
                    v8::Local<v8::Value> raw_strategy,
                    bindings::ExceptionState& exception_state);

 private:
  friend class WritableStreamDefaultWriter;

  using PromiseQueue = HeapDeque<bindings::ScriptPromiseResolver>;

  class PendingAbortRequest;

  // WritableStreamHasOperationMarkedInFlight().
  static bool HasOperationMarkedInFlight(const WritableStream* stream);
  // WritableStreamRejectCloseAndClosedPromiseIfNeeded().
  static void RejectCloseAndClosedPromiseIfNeeded(v8::Isolate* isolate,
                                                  WritableStream* stream);
  // Rejects each promise of `queue` with `e`.
  static void RejectPromises(v8::Isolate* isolate,
                             PromiseQueue* queue,
                             v8::Local<v8::Value> e);

  // [[backpressure]].
  bool has_backpressure_ = false;
  State state_ = kWritable;
  cppgc::Member<bindings::ScriptPromiseResolver> close_request_;
  cppgc::Member<bindings::ScriptPromiseResolver> in_flight_write_request_;
  cppgc::Member<bindings::ScriptPromiseResolver> in_flight_close_request_;
  cppgc::Member<PendingAbortRequest> pending_abort_request_;
  v8::TracedReference<v8::Value> stored_error_;
  cppgc::Member<WritableStreamDefaultController> writable_stream_controller_;
  cppgc::Member<WritableStreamDefaultWriter> writer_;
  PromiseQueue write_requests_;
};

}  // namespace mitto

#endif  // MITTO_DOM_WRITABLE_STREAM_H_
