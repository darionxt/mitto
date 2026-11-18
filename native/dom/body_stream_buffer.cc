#include "mitto/dom/body_stream_buffer.h"

#include <algorithm>
#include <utility>

#include "base/auto_reset.h"
#include "base/check.h"
#include "base/compiler_specific.h"
#include "base/notreached.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/bindings/runtime_features.h"
#include "mitto/bindings/script_promise.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/abort_signal.h"
#include "mitto/dom/readable_byte_stream_controller.h"
#include "mitto/dom/readable_stream.h"
#include "mitto/dom/readable_stream_byob_request.h"
#include "mitto/dom/readable_stream_bytes_consumer.h"
#include "mitto/dom/stream_chunks.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {
namespace {

ReadableByteStreamController* ByteController(ReadableStream* stream) {
  ReadableStreamController* const controller = stream->GetController();
  CHECK(controller->IsByteStreamController());
  return static_cast<ReadableByteStreamController*>(controller);
}

// What an aborted body errors with: the signal's reason under
// ForwardReasonToFetchBodyAbort, and before it an AbortError of Blink's own.
v8::Local<v8::Value> AbortReason(v8::Isolate* isolate, AbortSignal* signal) {
  bindings::PerIsolateData* const data =
      bindings::PerIsolateData::From(isolate);
  if (signal && data->features().IsEnabled(
                    bindings::RuntimeFeature::kForwardReasonToFetchBodyAbort)) {
    return signal->Reason();
  }
  return data->dom_exception_hooks().create(isolate, "AbortError",
                                            "BodyStreamBuffer was aborted");
}

}  // namespace

// What a FetchDataLoader that StartLoading() started tells: the buffer stops
// loading, and the client hears.
class BodyStreamBuffer::LoaderClient final
    : public cppgc::GarbageCollected<LoaderClient>,
      public FetchDataLoader::Client {
 public:
  LoaderClient(BodyStreamBuffer* buffer, FetchDataLoader::Client* client)
      : buffer_(buffer), client_(client) {}

  void DidFetchDataLoadedArrayBuffer(std::string bytes) override {
    buffer_->EndLoading();
    client_->DidFetchDataLoadedArrayBuffer(std::move(bytes));
  }

  void DidFetchDataLoadedString(std::u16string string) override {
    buffer_->EndLoading();
    client_->DidFetchDataLoadedString(std::move(string));
  }

  void DidFetchDataLoadFailed() override {
    buffer_->EndLoading();
    client_->DidFetchDataLoadFailed();
  }

  void Abort(v8::Local<v8::Value> reason) override { NOTREACHED(); }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(buffer_);
    visitor->Trace(client_);
    FetchDataLoader::Client::Trace(visitor);
  }

 private:
  const cppgc::Member<BodyStreamBuffer> buffer_;
  const cppgc::Member<FetchDataLoader::Client> client_;
};

// The abort algorithm of the stream, which holds the buffer weakly.
class BodyStreamBuffer::AbortAlgorithm final : public AbortSignal::Algorithm {
 public:
  explicit AbortAlgorithm(BodyStreamBuffer* buffer) : buffer_(buffer) {}

  void Run() override {
    if (BodyStreamBuffer* const buffer = buffer_.Get()) {
      buffer->Abort();
    }
  }

  void Trace(cppgc::Visitor* visitor) const override {
    AbortSignal::Algorithm::Trace(visitor);
    visitor->Trace(buffer_);
  }

 private:
  const cppgc::WeakMember<BodyStreamBuffer> buffer_;
};

// The abort algorithm of a load, which holds the buffer and the client weakly.
class BodyStreamBuffer::AbortLoaderAlgorithm final
    : public AbortSignal::Algorithm {
 public:
  AbortLoaderAlgorithm(BodyStreamBuffer* buffer,
                       FetchDataLoader::Client* client)
      : buffer_(buffer), client_(client) {}

  void Run() override {
    if (BodyStreamBuffer* const buffer = buffer_.Get()) {
      buffer->AbortLoader(client_.Get());
    }
  }

  void Trace(cppgc::Visitor* visitor) const override {
    AbortSignal::Algorithm::Trace(visitor);
    visitor->Trace(buffer_);
    visitor->Trace(client_);
  }

 private:
  const cppgc::WeakMember<BodyStreamBuffer> buffer_;
  const cppgc::WeakMember<FetchDataLoader::Client> client_;
};

// static
BodyStreamBuffer* BodyStreamBuffer::Create(v8::Isolate* isolate,
                                           BytesConsumer* consumer,
                                           AbortSignal* signal) {
  auto* const buffer = bindings::MakeGarbageCollected<BodyStreamBuffer>(
      isolate, consumer, signal);
  buffer->Init();
  return buffer;
}

BodyStreamBuffer::BodyStreamBuffer(v8::Isolate* isolate,
                                   BytesConsumer* consumer,
                                   AbortSignal* signal)
    : isolate_(isolate),
      consumer_(consumer),
      signal_(signal),
      made_from_readable_stream_(false) {}

BodyStreamBuffer::BodyStreamBuffer(v8::Isolate* isolate, ReadableStream* stream)
    : isolate_(isolate), stream_(stream), made_from_readable_stream_(true) {
  DCHECK(stream_);
}

BodyStreamBuffer::~BodyStreamBuffer() = default;

void BodyStreamBuffer::Init() {
  DCHECK(consumer_);
  stream_ = ReadableStream::CreateByteStream(isolate_, this);
  stream_broken_ = !stream_;
  consumer_->SetClient(this);
  if (signal_) {
    if (signal_->Aborted()) {
      Abort();
    } else {
      signal_->AddAlgorithm(
          bindings::MakeGarbageCollected<AbortAlgorithm>(this));
    }
  }
  OnStateChange();
}

std::optional<std::string> BodyStreamBuffer::DrainAsBytes(
    bindings::ExceptionState& exception_state) {
  DCHECK(!IsStreamLocked());
  DCHECK(!IsStreamDisturbed());
  if (IsStreamClosed() || IsStreamErrored() || stream_broken_) {
    return std::nullopt;
  }
  if (made_from_readable_stream_ || !consumer_) {
    return std::nullopt;
  }
  std::optional<std::string> bytes = consumer_->DrainAsBytes();
  if (bytes) {
    CloseAndLockAndDisturb(exception_state);
  }
  return bytes;
}

void BodyStreamBuffer::StartLoading(FetchDataLoader* loader,
                                    FetchDataLoader::Client* client,
                                    bindings::ExceptionState& exception_state) {
  DCHECK(!loader_);
  DCHECK(!keep_alive_);
  if (signal_) {
    if (signal_->Aborted()) {
      AbortLoader(client);
      return;
    }
    signal_->AddAlgorithm(
        bindings::MakeGarbageCollected<AbortLoaderAlgorithm>(this, client));
  }
  loader_ = loader;
  BytesConsumer* const handle = ReleaseHandle(exception_state);
  if (exception_state.HadException()) {
    return;
  }
  keep_alive_ = this;
  loader->Start(handle,
                bindings::MakeGarbageCollected<LoaderClient>(this, client));
}

v8::Local<v8::Promise> BodyStreamBuffer::Pull(
    ReadableByteStreamController* controller,
    bindings::ExceptionState& exception_state) {
  if (!consumer_) {
    return bindings::ResolvedUndefinedPromise(isolate_);
  }
  if (stream_needs_more_) {
    return bindings::ResolvedUndefinedPromise(isolate_);
  }
  stream_needs_more_ = true;
  if (!in_process_data_) {
    ProcessData(exception_state);
  }
  return bindings::ResolvedUndefinedPromise(isolate_);
}

v8::Local<v8::Promise> BodyStreamBuffer::Cancel(v8::Local<v8::Value> reason) {
  ReadableByteStreamController::Close(isolate_, ByteController(stream_));
  CancelConsumer();
  return bindings::ResolvedUndefinedPromise(isolate_);
}

void BodyStreamBuffer::OnStateChange() {
  if (!consumer_) {
    return;
  }
  bindings::ExceptionState exception_state(isolate_);
  switch (consumer_->GetPublicState()) {
    case BytesConsumer::PublicState::kReadableOrWaiting:
      break;
    case BytesConsumer::PublicState::kClosed:
      Close(exception_state);
      return;
    case BytesConsumer::PublicState::kErrored:
      GetError();
      return;
  }
  ProcessData(exception_state);
}

bool BodyStreamBuffer::IsStreamReadable() const {
  return stream_->IsReadable();
}

bool BodyStreamBuffer::IsStreamClosed() const {
  return stream_->IsClosed();
}

bool BodyStreamBuffer::IsStreamErrored() const {
  return stream_->IsErrored();
}

bool BodyStreamBuffer::IsStreamLocked() const {
  return stream_->IsLocked();
}

bool BodyStreamBuffer::IsStreamDisturbed() const {
  return stream_->IsDisturbed();
}

void BodyStreamBuffer::CloseAndLockAndDisturb(
    bindings::ExceptionState& exception_state) {
  DCHECK(!stream_broken_);
  if (IsStreamReadable()) {
    // Note that the stream cannot be "draining", because it doesn't have the
    // internal buffer.
    Close(exception_state);
  }
  stream_->LockAndDisturb(isolate_);
}

void BodyStreamBuffer::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(stream_);
  visitor->Trace(consumer_);
  visitor->Trace(loader_);
  visitor->Trace(signal_);
  UnderlyingByteSourceBase::Trace(visitor);
  BytesConsumer::Client::Trace(visitor);
}

void BodyStreamBuffer::Abort() {
  const v8::Local<v8::Value> reason = AbortReason(isolate_, signal_.Get());
  CHECK(!reason.IsEmpty());
  ReadableByteStreamController::Error(isolate_, ByteController(stream_),
                                      reason);
  CancelConsumer();
}

void BodyStreamBuffer::AbortLoader(FetchDataLoader::Client* client) {
  if (!client) {
    return;
  }
  client->Abort(AbortReason(isolate_, signal_.Get()));
}

void BodyStreamBuffer::Close(bindings::ExceptionState& exception_state) {
  // Close() can be called during construction, in which case `stream_` will
  // not be set yet.
  if (stream_) {
    const v8::TryCatch try_catch(isolate_);
    bindings::ExceptionState pass_through(isolate_);
    stream_->CloseStream(isolate_, pass_through);
    if (try_catch.HasCaught()) {
      return;
    }
  }
  CancelConsumer();
}

void BodyStreamBuffer::GetError() {
  ReadableByteStreamController::Error(
      isolate_, ByteController(stream_),
      v8::Exception::TypeError(
          v8::String::NewFromUtf8Literal(isolate_, "network error")));
  CancelConsumer();
}

void BodyStreamBuffer::CancelConsumer() {
  if (consumer_) {
    consumer_->Cancel();
    consumer_ = nullptr;
  }
}

void BodyStreamBuffer::ProcessData(bindings::ExceptionState& exception_state) {
  DCHECK(consumer_);
  DCHECK(!in_process_data_);
  const base::AutoReset<bool> auto_reset(&in_process_data_, true);
  while (stream_needs_more_) {
    base::span<const char> buffer;
    auto result = consumer_->BeginRead(buffer);
    if (result == BytesConsumer::Result::kShouldWait) {
      return;
    }
    v8::Local<v8::Uint8Array> array;
    bool byob = false;
    if (result == BytesConsumer::Result::kOk) {
      ReadableByteStreamController* const byte_controller =
          ByteController(stream_);
      if (ReadableStreamBYOBRequest* const request =
              ReadableByteStreamController::GetBYOBRequest(byte_controller)) {
        const v8::Local<v8::ArrayBufferView> view = request->View();
        buffer = buffer.first(std::min(view->ByteLength(), buffer.size()));
        // SAFETY: the view's bytes lie at its offset in its buffer.
        UNSAFE_BUFFERS(base::span(static_cast<char*>(view->Buffer()->Data()) +
                                      view->ByteOffset(),
                                  view->ByteLength()))
            .copy_prefix_from(buffer);
        byob = true;
      }
      if (!byob) {
        array = NewUint8Array(isolate_, base::as_bytes(buffer));
      }
      result = consumer_->EndRead(buffer.size());
    }
    switch (result) {
      case BytesConsumer::Result::kOk:
      case BytesConsumer::Result::kDone:
        if (!array.IsEmpty() || byob) {
          // Clear |stream_needs_more_| in order to detect a pull call.
          stream_needs_more_ = false;
          const v8::TryCatch try_catch(isolate_);
          bindings::ExceptionState pass_through(isolate_);
          ReadableByteStreamController* const byte_controller =
              ByteController(stream_);
          if (byob) {
            ReadableByteStreamController::Respond(isolate_, byte_controller,
                                                  buffer.size(), pass_through);
          } else {
            ReadableByteStreamController::Enqueue(isolate_, byte_controller,
                                                  array, pass_through);
          }
          if (try_catch.HasCaught()) {
            return;
          }
        }
        if (result == BytesConsumer::Result::kDone) {
          Close(exception_state);
          return;
        }
        // If |stream_needs_more_| is true, it means that pull is called and
        // the stream needs more data even if the desired size is not
        // positive.
        if (!stream_needs_more_) {
          const std::optional<double> desired_size =
              ReadableByteStreamController::GetDesiredSize(
                  ByteController(stream_));
          DCHECK(desired_size.has_value());
          stream_needs_more_ = desired_size.value() > 0;
        }
        break;
      case BytesConsumer::Result::kShouldWait:
        NOTREACHED();
      case BytesConsumer::Result::kError:
        GetError();
        return;
    }
  }
}

void BodyStreamBuffer::EndLoading() {
  if (!loader_) {
    DCHECK(!keep_alive_);
    return;
  }
  keep_alive_.Clear();
  loader_ = nullptr;
}

BodyStreamBuffer* BodyStreamBuffer::CreateProxy(
    bindings::ExceptionState& exception_state) {
  // ReleaseHandle() locks this buffer's stream and, unless the body is made
  // from a ReadableStream, also closes and disturbs it.
  BytesConsumer* const consumer = ReleaseHandle(exception_state);
  if (!consumer) {
    return nullptr;
  }
  BodyStreamBuffer* const proxy =
      BodyStreamBuffer::Create(isolate_, consumer, signal_.Get());
  if (!made_from_readable_stream_) {
    return proxy;
  }
  // For a stream-backed body ReleaseHandle() returns a consumer that acquires
  // its own reader, which locks the stream but does not disturb it until a
  // read happens; "create a proxy" disturbs synchronously. This has to come
  // after ReleaseHandle(), which DCHECKs that the stream is not yet disturbed.
  stream_->MarkDisturbed();
  // Like this body, the proxy has no source and an unknown length, so it has
  // to be made from a ReadableStream as well.
  return bindings::MakeGarbageCollected<BodyStreamBuffer>(isolate_,
                                                          proxy->Stream());
}

BytesConsumer* BodyStreamBuffer::ReleaseHandle(
    bindings::ExceptionState& exception_state) {
  DCHECK(!IsStreamLocked());
  DCHECK(!IsStreamDisturbed());
  if (stream_broken_) {
    exception_state.ThrowDOMException(
        "InvalidStateError",
        "Body stream has suffered a fatal error and cannot be inspected");
    return nullptr;
  }
  if (made_from_readable_stream_) {
    return bindings::MakeGarbageCollected<ReadableStreamBytesConsumer>(
        isolate_, stream_.Get());
  }
  // We need to call these before calling CloseAndLockAndDisturb.
  const bool is_closed = IsStreamClosed();
  const bool is_errored = IsStreamErrored();
  BytesConsumer* const consumer = consumer_.Get();
  consumer_ = nullptr;
  CloseAndLockAndDisturb(exception_state);
  if (is_closed) {
    // Note that the stream cannot be "draining", because it doesn't have the
    // internal buffer.
    return BytesConsumer::CreateClosed();
  }
  if (is_errored) {
    return BytesConsumer::CreateErrored();
  }
  DCHECK(consumer);
  consumer->ClearClient();
  return consumer;
}

}  // namespace mitto
