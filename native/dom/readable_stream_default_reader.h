// The Streams Standard's ReadableStreamDefaultReader, as Blink's.
//
// Upstream: third_party/blink/renderer/core/streams/readable_stream_default_reader.h
// Upstream: third_party/blink/renderer/core/streams/readable_stream_default_reader.cc

#ifndef MITTO_DOM_READABLE_STREAM_DEFAULT_READER_H_
#define MITTO_DOM_READABLE_STREAM_DEFAULT_READER_H_

#include "mitto/dom/heap_deque.h"
#include "mitto/dom/readable_stream_generic_reader.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-promise.h"

namespace v8 {
class Isolate;
class Value;
}  // namespace v8

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class ReadableStream;
class ReadRequest;

class ReadableStreamDefaultReader final : public ReadableStreamGenericReader {
 public:
  // `new ReadableStreamDefaultReader(stream)`.
  static ReadableStreamDefaultReader* Create(
      ReadableStream* stream,
      bindings::ExceptionState& exception_state);

  ReadableStreamDefaultReader();
  ~ReadableStreamDefaultReader() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  bool IsDefaultReader() const override;
  bool IsBYOBReader() const override;

  // The members of the interface.
  v8::Local<v8::Promise> Read(bindings::ExceptionState& exception_state);
  void ReleaseLock(bindings::ExceptionState& exception_state);

  // SetUpReadableStreamDefaultReader().
  static void SetUpDefaultReader(v8::Isolate* isolate,
                                 ReadableStreamDefaultReader* reader,
                                 ReadableStream* stream,
                                 bindings::ExceptionState& exception_state);

  // The abstract operations of default readers.
  //
  // ReadableStreamDefaultReaderRead().
  static void Read(v8::Isolate* isolate,
                   ReadableStreamDefaultReader* reader,
                   ReadRequest* read_request,
                   bindings::ExceptionState& exception_state);
  // ReadableStreamDefaultReaderErrorReadRequests().
  static void ErrorReadRequests(v8::Isolate* isolate,
                                ReadableStreamDefaultReader* reader,
                                v8::Local<v8::Value> e);
  // ReadableStreamDefaultReaderRelease().
  static void Release(v8::Isolate* isolate,
                      ReadableStreamDefaultReader* reader);

 private:
  friend class ByteStreamTeeEngine;
  friend class ReadableByteStreamController;
  friend class ReadableStreamController;
  friend class ReadableStreamDefaultController;
  friend class ReadableStream;

  class DefaultReaderReadRequest;

  HeapDeque<ReadRequest> read_requests_;
};

}  // namespace mitto

#endif  // MITTO_DOM_READABLE_STREAM_DEFAULT_READER_H_
