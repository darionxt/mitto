// The Streams Standard's WritableStreamDefaultWriter, as Blink's.
//
// Upstream: third_party/blink/renderer/core/streams/writable_stream_default_writer.h
// Upstream: third_party/blink/renderer/core/streams/writable_stream_default_writer.cc

#ifndef MITTO_DOM_WRITABLE_STREAM_DEFAULT_WRITER_H_
#define MITTO_DOM_WRITABLE_STREAM_DEFAULT_WRITER_H_

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

class WritableStream;

class WritableStreamDefaultWriter final : public bindings::ScriptWrappable {
 public:
  // `new WritableStreamDefaultWriter(stream)`.
  static WritableStreamDefaultWriter* Create(
      WritableStream* stream,
      bindings::ExceptionState& exception_state);

  WritableStreamDefaultWriter();
  ~WritableStreamDefaultWriter() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The steps of the constructor: SetUpWritableStreamDefaultWriter().
  static void SetUp(v8::Isolate* isolate,
                    WritableStreamDefaultWriter* writer,
                    WritableStream* stream,
                    bindings::ExceptionState& exception_state);

  // The members of the interface.
  v8::Local<v8::Promise> Closed() const;
  v8::Local<v8::Value> DesiredSize(
      bindings::ExceptionState& exception_state) const;
  v8::Local<v8::Promise> Ready() const;
  v8::Local<v8::Promise> Abort(
      const std::optional<v8::Local<v8::Value>>& reason,
      bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> Close(bindings::ExceptionState& exception_state);
  void ReleaseLock();
  v8::Local<v8::Promise> Write(
      const std::optional<v8::Local<v8::Value>>& chunk,
      bindings::ExceptionState& exception_state);

  // For WritableStream.
  //
  // WritableStreamDefaultWriterEnsureReadyPromiseRejected().
  static void EnsureReadyPromiseRejected(v8::Isolate* isolate,
                                         WritableStreamDefaultWriter* writer,
                                         v8::Local<v8::Value> error);

  // For ReadableStream.
  //
  // WritableStreamDefaultWriterCloseWithErrorPropagation().
  static v8::Local<v8::Promise> CloseWithErrorPropagation(
      v8::Isolate* isolate,
      WritableStreamDefaultWriter* writer);
  // WritableStreamDefaultWriterRelease().
  static void Release(v8::Isolate* isolate,
                      WritableStreamDefaultWriter* writer);
  // WritableStreamDefaultWriterWrite().
  static v8::Local<v8::Promise> Write(
      v8::Isolate* isolate,
      WritableStreamDefaultWriter* writer,
      v8::Local<v8::Value> chunk,
      bindings::ExceptionState& exception_state);

  // Not part of the standard.
  bindings::ScriptPromiseResolver* ClosedResolver() {
    return closed_resolver_.Get();
  }
  bindings::ScriptPromiseResolver* ReadyResolver() {
    return ready_resolver_.Get();
  }
  WritableStream* OwnerWritableStream() { return owner_writable_stream_.Get(); }
  // GetDesiredSize() without making a value: nullopt for null.
  std::optional<double> GetDesiredSizeInternal() const;
  void ResetReadyPromise(v8::Isolate* isolate);

 private:
  // WritableStreamDefaultWriterAbort().
  static v8::Local<v8::Promise> Abort(v8::Isolate* isolate,
                                      WritableStreamDefaultWriter* writer,
                                      v8::Local<v8::Value> reason);
  // WritableStreamDefaultWriterClose().
  static v8::Local<v8::Promise> Close(v8::Isolate* isolate,
                                      WritableStreamDefaultWriter* writer);
  // WritableStreamDefaultWriterEnsureClosedPromiseRejected().
  static void EnsureClosedPromiseRejected(v8::Isolate* isolate,
                                          WritableStreamDefaultWriter* writer,
                                          v8::Local<v8::Value> error);
  // WritableStreamDefaultWriterGetDesiredSize().
  static v8::Local<v8::Value> GetDesiredSize(
      v8::Isolate* isolate,
      const WritableStreamDefaultWriter* writer);

  cppgc::Member<WritableStream> owner_writable_stream_;
  // [[closedPromise]] and [[readyPromise]].
  cppgc::Member<bindings::ScriptPromiseResolver> closed_resolver_;
  cppgc::Member<bindings::ScriptPromiseResolver> ready_resolver_;
};

}  // namespace mitto

#endif  // MITTO_DOM_WRITABLE_STREAM_DEFAULT_WRITER_H_
