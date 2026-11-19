// What a ReadableStream's default reader and BYOB reader share: their stream,
// `closed` and cancel(), as Blink's ReadableStreamGenericReader.
//
// Upstream: third_party/blink/renderer/core/streams/readable_stream_generic_reader.h
// Upstream: third_party/blink/renderer/core/streams/readable_stream_generic_reader.cc

#ifndef MITTO_DOM_READABLE_STREAM_GENERIC_READER_H_
#define MITTO_DOM_READABLE_STREAM_GENERIC_READER_H_

#include <optional>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
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
class ScriptPromiseResolver;
}  // namespace bindings

class ReadableStream;

class ReadableStreamGenericReader : public bindings::ScriptWrappable {
 public:
  ReadableStreamGenericReader();
  ~ReadableStreamGenericReader() override;

  virtual bool IsDefaultReader() const = 0;
  virtual bool IsBYOBReader() const = 0;

  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface mixin.
  v8::Local<v8::Promise> Closed() const;
  v8::Local<v8::Promise> Cancel(
      const std::optional<v8::Local<v8::Value>>& reason,
      bindings::ExceptionState& exception_state);

  // The abstract operations of readers.
  //
  // ReadableStreamReaderGenericRelease().
  static void GenericRelease(v8::Isolate* isolate,
                             ReadableStreamGenericReader* reader);
  // ReadableStreamReaderGenericCancel().
  static v8::Local<v8::Promise> GenericCancel(
      v8::Isolate* isolate,
      ReadableStreamGenericReader* reader,
      v8::Local<v8::Value> reason);
  // ReadableStreamReaderGenericInitialize().
  static void GenericInitialize(v8::Isolate* isolate,
                                ReadableStreamGenericReader* reader,
                                ReadableStream* stream);

  bindings::ScriptPromiseResolver* ClosedResolver() const {
    return closed_resolver_.Get();
  }
  ReadableStream* OwnerReadableStream() const {
    return owner_readable_stream_.Get();
  }

 private:
  friend class PipeToEngine;
  friend class ReadableStreamDefaultController;

  cppgc::Member<bindings::ScriptPromiseResolver> closed_resolver_;

 protected:
  cppgc::Member<ReadableStream> owner_readable_stream_;
};

}  // namespace mitto

#endif  // MITTO_DOM_READABLE_STREAM_GENERIC_READER_H_
