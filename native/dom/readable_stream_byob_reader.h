// The Streams Standard's ReadableStreamBYOBReader, as Blink's: reads of a byte
// stream into a view that the page gives.
//
// Upstream: third_party/blink/renderer/core/streams/readable_stream_byob_reader.h
// Upstream: third_party/blink/renderer/core/streams/readable_stream_byob_reader.cc

#ifndef MITTO_DOM_READABLE_STREAM_BYOB_READER_H_
#define MITTO_DOM_READABLE_STREAM_BYOB_READER_H_

#include <stdint.h>

#include "mitto/dom/heap_deque.h"
#include "mitto/dom/readable_stream_generic_reader.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-promise.h"
#include "v8/include/v8-typed-array.h"

namespace v8 {
class Isolate;
class Value;
}  // namespace v8

namespace mitto {

namespace bindings {
class ExceptionState;
struct ReadableStreamBYOBReaderReadOptions;
}  // namespace bindings

class ReadableStream;
class ReadIntoRequest;

class ReadableStreamBYOBReader final : public ReadableStreamGenericReader {
 public:
  // `new ReadableStreamBYOBReader(stream)`.
  static ReadableStreamBYOBReader* Create(
      ReadableStream* stream,
      bindings::ExceptionState& exception_state);

  ReadableStreamBYOBReader();
  ~ReadableStreamBYOBReader() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  bool IsDefaultReader() const override;
  bool IsBYOBReader() const override;

  // The members of the interface.
  v8::Local<v8::Promise> Read(
      v8::Local<v8::ArrayBufferView> view,
      const bindings::ReadableStreamBYOBReaderReadOptions& options,
      bindings::ExceptionState& exception_state);
  void ReleaseLock(bindings::ExceptionState& exception_state);

  // SetUpReadableStreamBYOBReader().
  static void SetUpBYOBReader(v8::Isolate* isolate,
                              ReadableStreamBYOBReader* reader,
                              ReadableStream* stream,
                              bindings::ExceptionState& exception_state);

 private:
  friend class ByteStreamTeeEngine;
  friend class PipeToEngine;
  friend class ReadableByteStreamController;
  friend class ReadableStream;

  class BYOBReaderReadIntoRequest;

  // ReadableStreamBYOBReaderRead().
  static void Read(v8::Isolate* isolate,
                   ReadableStreamBYOBReader* reader,
                   v8::Local<v8::ArrayBufferView> view,
                   uint64_t min,
                   ReadIntoRequest* read_into_request,
                   bindings::ExceptionState& exception_state);
  // ReadableStreamBYOBReaderErrorReadIntoRequests().
  static void ErrorReadIntoRequests(v8::Isolate* isolate,
                                    ReadableStreamBYOBReader* reader,
                                    v8::Local<v8::Value> e);
  // ReadableStreamBYOBReaderRelease().
  static void Release(v8::Isolate* isolate, ReadableStreamBYOBReader* reader);

  HeapDeque<ReadIntoRequest> read_into_requests_;
};

}  // namespace mitto

#endif  // MITTO_DOM_READABLE_STREAM_BYOB_READER_H_
