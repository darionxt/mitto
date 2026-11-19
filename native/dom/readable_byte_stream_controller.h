// The Streams Standard's ReadableByteStreamController, as Blink's: a byte
// stream's queue of buffers and its pending reads into views.
//
// Upstream: third_party/blink/renderer/core/streams/readable_byte_stream_controller.h
// Upstream: third_party/blink/renderer/core/streams/readable_byte_stream_controller.cc

#ifndef MITTO_DOM_READABLE_BYTE_STREAM_CONTROLLER_H_
#define MITTO_DOM_READABLE_BYTE_STREAM_CONTROLLER_H_

#include <stddef.h>
#include <stdint.h>

#include <optional>

#include "mitto/dom/heap_deque.h"
#include "mitto/dom/readable_stream_controller.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-promise.h"
#include "v8/include/v8-traced-handle.h"
#include "v8/include/v8-typed-array.h"

namespace v8 {
class Isolate;
class Value;
}  // namespace v8

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class ReadableStream;
class ReadableStreamBYOBRequest;
class ReadIntoRequest;
class ReadRequest;
class StreamAlgorithm;
class StreamStartAlgorithm;

class ReadableByteStreamController final : public ReadableStreamController {
 public:
  ReadableByteStreamController();
  ~ReadableByteStreamController() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  ReadableStreamBYOBRequest* ByobRequest();
  std::optional<double> DesiredSize();
  void Close(bindings::ExceptionState& exception_state);
  void Enqueue(v8::Local<v8::ArrayBufferView> chunk,
               bindings::ExceptionState& exception_state);
  void Error(const std::optional<v8::Local<v8::Value>>& e);

  // ReadableByteStreamControllerGetBYOBRequest().
  static ReadableStreamBYOBRequest* GetBYOBRequest(
      ReadableByteStreamController* controller);
  // ReadableByteStreamControllerGetDesiredSize().
  static std::optional<double> GetDesiredSize(
      ReadableByteStreamController* controller);

  bool IsByteStreamController() const override;
  bool IsDefaultController() const override;

  // [[CancelSteps]], [[PullSteps]] and [[ReleaseSteps]].
  v8::Local<v8::Promise> CancelSteps(v8::Isolate* isolate,
                                     v8::Local<v8::Value> reason) override;
  void PullSteps(v8::Isolate* isolate,
                 ReadRequest* read_request,
                 bindings::ExceptionState& exception_state) override;
  void ReleaseSteps() override;

  // The abstract operations that the Runtime's own byte sources call, such as
  // a response's body.
  //
  // ReadableByteStreamControllerClose().
  static void Close(v8::Isolate* isolate,
                    ReadableByteStreamController* controller);
  // ReadableByteStreamControllerError().
  static void Error(v8::Isolate* isolate,
                    ReadableByteStreamController* controller,
                    v8::Local<v8::Value> e);
  // ReadableByteStreamControllerEnqueue().
  static void Enqueue(v8::Isolate* isolate,
                      ReadableByteStreamController* controller,
                      v8::Local<v8::ArrayBufferView> chunk,
                      bindings::ExceptionState& exception_state);
  // ReadableByteStreamControllerRespond().
  static void Respond(v8::Isolate* isolate,
                      ReadableByteStreamController* controller,
                      size_t bytes_written,
                      bindings::ExceptionState& exception_state);
  // ReadableByteStreamControllerRespondWithNewView().
  static void RespondWithNewView(v8::Isolate* isolate,
                                 ReadableByteStreamController* controller,
                                 v8::Local<v8::ArrayBufferView> view,
                                 bindings::ExceptionState& exception_state);

 private:
  friend class ByteStreamTeeEngine;
  friend class ReadableStream;
  friend class ReadableStreamBYOBReader;
  friend class ReadableStreamBYOBRequest;

  // A readable byte stream queue entry.
  struct QueueEntry final : public cppgc::GarbageCollected<QueueEntry> {
    QueueEntry(v8::Isolate* isolate,
               v8::Local<v8::ArrayBuffer> buffer,
               size_t byte_offset,
               size_t byte_length);
    v8::Local<v8::ArrayBuffer> Buffer(v8::Isolate* isolate) const {
      return buffer.Get(isolate);
    }
    void Trace(cppgc::Visitor* visitor) const;

    const v8::TracedReference<v8::ArrayBuffer> buffer;
    size_t byte_offset;
    size_t byte_length;
  };

  enum class ReaderType { kDefault, kBYOB, kNone };

  // A pull-into descriptor.
  struct PullIntoDescriptor final
      : public cppgc::GarbageCollected<PullIntoDescriptor> {
    // Makes a view of the descriptor's view type, as the view constructor of
    // the standard does: `length` counts elements.
    using ViewConstructorType = v8::Local<v8::ArrayBufferView> (*)(
        v8::Local<v8::ArrayBuffer> buffer,
        size_t byte_offset,
        size_t length);

    PullIntoDescriptor(v8::Isolate* isolate,
                       v8::Local<v8::ArrayBuffer> buffer,
                       size_t buffer_byte_length,
                       size_t byte_offset,
                       size_t byte_length,
                       size_t bytes_filled,
                       uint64_t minimum_fill,
                       size_t element_size,
                       ViewConstructorType view_constructor,
                       ReaderType reader_type);
    v8::Local<v8::ArrayBuffer> Buffer(v8::Isolate* isolate) const {
      return buffer.Get(isolate);
    }
    void SetBuffer(v8::Isolate* isolate, v8::Local<v8::ArrayBuffer> value) {
      buffer.Reset(isolate, value);
    }
    void Trace(cppgc::Visitor* visitor) const;

    v8::TracedReference<v8::ArrayBuffer> buffer;
    const size_t buffer_byte_length;
    size_t byte_offset;
    const size_t byte_length;
    size_t bytes_filled;
    const uint64_t minimum_fill;
    const size_t element_size;
    const ViewConstructorType view_constructor;
    ReaderType reader_type;
  };

  // ReadableByteStreamControllerEnqueueChunkToQueue().
  static void EnqueueChunkToQueue(v8::Isolate* isolate,
                                  ReadableByteStreamController* controller,
                                  v8::Local<v8::ArrayBuffer> buffer,
                                  size_t byte_offset,
                                  size_t byte_length);
  // ReadableByteStreamControllerEnqueueClonedChunkToQueue().
  static void EnqueueClonedChunkToQueue(
      v8::Isolate* isolate,
      ReadableByteStreamController* controller,
      v8::Local<v8::ArrayBuffer> buffer,
      size_t byte_offset,
      size_t byte_length);
  // ReadableByteStreamControllerEnqueueDetachedPullIntoToQueue().
  static void EnqueueDetachedPullIntoToQueue(
      v8::Isolate* isolate,
      ReadableByteStreamController* controller,
      PullIntoDescriptor* pull_into_descriptor);
  // ReadableByteStreamControllerProcessPullIntoDescriptorsUsingQueue().
  static void ProcessPullIntoDescriptorsUsingQueue(
      v8::Isolate* isolate,
      ReadableByteStreamController* controller);
  // ReadableByteStreamControllerProcessReadRequestsUsingQueue().
  static void ProcessReadRequestsUsingQueue(
      v8::Isolate* isolate,
      ReadableByteStreamController* controller,
      bindings::ExceptionState& exception_state);
  // ReadableByteStreamControllerCallPullIfNeeded().
  static void CallPullIfNeeded(v8::Isolate* isolate,
                               ReadableByteStreamController* controller);
  // ReadableByteStreamControllerShiftPendingPullInto().
  static PullIntoDescriptor* ShiftPendingPullInto(
      ReadableByteStreamController* controller);
  // ReadableByteStreamControllerShouldCallPull().
  static bool ShouldCallPull(ReadableByteStreamController* controller);
  // ReadableByteStreamControllerCommitPullIntoDescriptor().
  static void CommitPullIntoDescriptor(
      v8::Isolate* isolate,
      ReadableStream* stream,
      PullIntoDescriptor* pull_into_descriptor,
      bindings::ExceptionState& exception_state);
  // ReadableByteStreamControllerConvertPullIntoDescriptor().
  static v8::Local<v8::ArrayBufferView> ConvertPullIntoDescriptor(
      v8::Isolate* isolate,
      PullIntoDescriptor* pull_into_descriptor,
      bindings::ExceptionState& exception_state);
  // ReadableByteStreamControllerClearPendingPullIntos().
  static void ClearPendingPullIntos(ReadableByteStreamController* controller);
  // ReadableByteStreamControllerClearAlgorithms().
  static void ClearAlgorithms(ReadableByteStreamController* controller);
  // ReadableByteStreamControllerInvalidateBYOBRequest().
  static void InvalidateBYOBRequest(ReadableByteStreamController* controller);
  // SetUpReadableByteStreamController().
  static void SetUp(v8::Isolate* isolate,
                    ReadableStream* stream,
                    ReadableByteStreamController* controller,
                    StreamStartAlgorithm* start_algorithm,
                    StreamAlgorithm* pull_algorithm,
                    StreamAlgorithm* cancel_algorithm,
                    double high_water_mark,
                    size_t auto_allocate_chunk_size,
                    bindings::ExceptionState& exception_state);
  // SetUpReadableByteStreamControllerFromUnderlyingSource(), with what the
  // constructor read of the source as an UnderlyingSource dictionary: its
  // start(), pull() and cancel() (each a function or undefined), and its
  // autoAllocateChunkSize, nullopt when it is undefined.
  static void SetUpFromUnderlyingSource(
      v8::Isolate* isolate,
      ReadableStream* stream,
      v8::Local<v8::Object> underlying_source,
      v8::Local<v8::Value> start_method,
      v8::Local<v8::Value> pull_method,
      v8::Local<v8::Value> cancel_method,
      std::optional<uint64_t> auto_allocate_chunk_size,
      double high_water_mark,
      bindings::ExceptionState& exception_state);
  // ReadableByteStreamControllerFillHeadPullIntoDescriptor().
  static void FillHeadPullIntoDescriptor(
      ReadableByteStreamController* controller,
      size_t size,
      PullIntoDescriptor* pull_into_descriptor);
  // ReadableByteStreamControllerFillPullIntoDescriptorFromQueue().
  static bool FillPullIntoDescriptorFromQueue(
      v8::Isolate* isolate,
      ReadableByteStreamController* controller,
      PullIntoDescriptor* pull_into_descriptor,
      bindings::ExceptionState& exception_state);
  // ReadableByteStreamControllerFillReadRequestFromQueue().
  static void FillReadRequestFromQueue(
      v8::Isolate* isolate,
      ReadableByteStreamController* controller,
      ReadRequest* read_request,
      bindings::ExceptionState& exception_state);
  // ReadableByteStreamControllerPullInto().
  static void PullInto(v8::Isolate* isolate,
                       ReadableByteStreamController* controller,
                       v8::Local<v8::ArrayBufferView> view,
                       uint64_t min,
                       ReadIntoRequest* read_into_request,
                       bindings::ExceptionState& exception_state);
  // ReadableByteStreamControllerHandleQueueDrain().
  static void HandleQueueDrain(v8::Isolate* isolate,
                               ReadableByteStreamController* controller);
  // ResetQueue().
  static void ResetQueue(ReadableByteStreamController* controller);
  // ReadableByteStreamControllerRespondInClosedState().
  static void RespondInClosedState(v8::Isolate* isolate,
                                   ReadableByteStreamController* controller,
                                   PullIntoDescriptor* first_descriptor,
                                   bindings::ExceptionState& exception_state);
  // ReadableByteStreamControllerRespondInReadableState().
  static void RespondInReadableState(
      v8::Isolate* isolate,
      ReadableByteStreamController* controller,
      size_t bytes_written,
      PullIntoDescriptor* pull_into_descriptor,
      bindings::ExceptionState& exception_state);
  // ReadableByteStreamControllerRespondInternal().
  static void RespondInternal(v8::Isolate* isolate,
                              ReadableByteStreamController* controller,
                              size_t bytes_written,
                              bindings::ExceptionState& exception_state);
  // CanTransferArrayBuffer().
  static bool CanTransferArrayBuffer(v8::Local<v8::ArrayBuffer> buffer);
  // TransferArrayBuffer(): an empty handle if it threw.
  static v8::Local<v8::ArrayBuffer> TransferArrayBuffer(
      v8::Isolate* isolate,
      v8::Local<v8::ArrayBuffer> buffer,
      bindings::ExceptionState& exception_state);

  // autoAllocateChunkSize, 0 for undefined.
  size_t auto_allocate_chunk_size_ = 0u;
  cppgc::Member<ReadableStreamBYOBRequest> byob_request_;
  cppgc::Member<StreamAlgorithm> cancel_algorithm_;
  bool close_requested_ = false;
  bool pull_again_ = false;
  cppgc::Member<StreamAlgorithm> pull_algorithm_;
  bool pulling_ = false;
  HeapDeque<PullIntoDescriptor> pending_pull_intos_;
  HeapDeque<QueueEntry> queue_;
  double queue_total_size_ = 0;
  bool started_ = false;
  double strategy_high_water_mark_ = 0.0;
  cppgc::Member<ReadableStream> controlled_readable_stream_;
};

}  // namespace mitto

#endif  // MITTO_DOM_READABLE_BYTE_STREAM_CONTROLLER_H_
