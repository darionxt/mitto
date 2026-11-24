// The body of a Response or a Request, as Blink's BodyStreamBuffer: a byte
// stream over a BytesConsumer, the stream's underlying source, which reads as
// the stream is pulled, or a stream that a script made. text(), json() and
// arrayBuffer() read the consumer itself, through a FetchDataLoader, once
// they have closed, locked and disturbed the stream. A body that a request's
// signal aborts errors with the signal's reason.
//
// Upstream: third_party/blink/renderer/core/fetch/body_stream_buffer.h
// Upstream: third_party/blink/renderer/core/fetch/body_stream_buffer.cc

#ifndef MITTO_DOM_BODY_STREAM_BUFFER_H_
#define MITTO_DOM_BODY_STREAM_BUFFER_H_

#include <optional>
#include <string>

#include "base/functional/function_ref.h"
#include "base/memory/raw_ptr.h"
#include "mitto/dom/bytes_consumer.h"
#include "mitto/dom/fetch_data_loader.h"
#include "mitto/dom/underlying_byte_source_base.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/persistent.h"
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

class AbortSignal;
class ExecutionContext;
class ReadableStream;

class BodyStreamBuffer final : public UnderlyingByteSourceBase,
                               public BytesConsumer::Client {
 public:
  // A body of the bytes of `consumer`, which `signal`, if any, aborts.
  static BodyStreamBuffer* Create(v8::Isolate* isolate,
                                  BytesConsumer* consumer,
                                  AbortSignal* signal);

  // Use Create(): the steps of Blink's constructor before Init().
  BodyStreamBuffer(v8::Isolate* isolate,
                   BytesConsumer* consumer,
                   AbortSignal* signal);
  // A body made from `stream`, which a script made.
  BodyStreamBuffer(v8::Isolate* isolate, ReadableStream* stream);
  ~BodyStreamBuffer() override;

  ReadableStream* Stream() const { return stream_.Get(); }

  // All of its bytes, once its stream is closed, locked and disturbed, for a
  // body of bytes that a script gave whole, as Blink's DrainAsFormData()
  // gives them; nullopt for any other, which is left as it was.
  std::optional<std::string> DrainAsBytes(
      bindings::ExceptionState& exception_state);
  // Reads all of it with `loader`, which tells `client`. The stream must be
  // neither locked nor disturbed.
  void StartLoading(FetchDataLoader* loader,
                    FetchDataLoader::Client* client,
                    bindings::ExceptionState& exception_state);
  // "Create a proxy": a new body that reads this one, whose stream is then
  // locked and disturbed.
  BodyStreamBuffer* CreateProxy(bindings::ExceptionState& exception_state);
  // Splits the body into two that each give all of it, as clone() does,
  // leaving this one locked: a stream's own tee for a body made from a
  // ReadableStream, and BytesConsumerTee() for any other. Throws, and leaves
  // both null, for a stream in a state that cannot be known.
  void Tee(BodyStreamBuffer** branch1,
           BodyStreamBuffer** branch2,
           bindings::ExceptionState& exception_state);

  // UnderlyingByteSourceBase:
  v8::Local<v8::Promise> Pull(
      ReadableByteStreamController* controller,
      bindings::ExceptionState& exception_state) override;
  v8::Local<v8::Promise> Cancel(v8::Local<v8::Value> reason) override;

  // BytesConsumer::Client:
  void OnStateChange() override;

  bool IsStreamReadable() const;
  bool IsStreamClosed() const;
  bool IsStreamErrored() const;
  bool IsStreamLocked() const;
  bool IsStreamDisturbed() const;
  // Closes the stream if it is readable, and locks and disturbs it.
  void CloseAndLockAndDisturb(bindings::ExceptionState& exception_state);
  bool IsMadeFromReadableStream() const { return made_from_readable_stream_; }

  void Trace(cppgc::Visitor* visitor) const override;

 private:
  class LoaderClient;
  class AbortAlgorithm;
  class AbortLoaderAlgorithm;

  void Init();
  BytesConsumer* ReleaseHandle(bindings::ExceptionState& exception_state);
  void Abort();
  void AbortLoader(FetchDataLoader::Client* client);
  void Close(bindings::ExceptionState& exception_state);
  void GetError();
  void CancelConsumer();
  void ProcessData(bindings::ExceptionState& exception_state);
  void EndLoading();
  // What Blink's ContextDestroyed() of the buffer and of its LoaderClient do:
  // the load and the consumer stop, and it no longer keeps itself alive.
  void ContextDestroyed();
  void StopLoading();
  // Whether its context is still there, as Blink's script state is valid.
  bool HasContext() const;
  // Sets the context, which the constructors set to the current one.
  void SetContext(ExecutionContext* context);
  // Runs `run` in its context, or in the current one if that has gone.
  void RunInOwnContext(base::FunctionRef<void()> run);

  const raw_ptr<v8::Isolate> isolate_;
  // The context that it was made in, or that of the body it comes from.
  cppgc::WeakMember<ExecutionContext> context_;
  // Whether that context had gone before this was made, which Blink's
  // observer of it then never hears of.
  bool made_without_context_ = false;
  cppgc::Member<ReadableStream> stream_;
  cppgc::Member<BytesConsumer> consumer_;
  // While it is loading, it keeps itself alive, as Blink's keep_alive_ does.
  cppgc::Persistent<BodyStreamBuffer> keep_alive_;
  cppgc::Member<FetchDataLoader> loader_;
  cppgc::Member<LoaderClient> loader_client_;
  // The signal of the request that brought the body, if any.
  cppgc::Member<AbortSignal> signal_;
  // Whether the stream needs more data even if the desired size is not
  // positive.
  bool stream_needs_more_ = false;
  // Whether ProcessData() is running.
  bool in_process_data_ = false;
  // Whether the stream could not be made, or a tee left it in an unknown
  // state.
  bool stream_broken_ = false;
  const bool made_from_readable_stream_;
};

}  // namespace mitto

#endif  // MITTO_DOM_BODY_STREAM_BUFFER_H_
